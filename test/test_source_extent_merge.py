#!/usr/bin/env python3
"""Exercise declaration-extent reconciliation across provider boundaries."""

from __future__ import annotations

import json
from typing import Mapping, Optional, Tuple

from archbird.native import Project, Source, query_map_json


def provider_bundle(
    project: Project,
    *,
    producer: str,
    name: str,
    anchor: Tuple[int, int],
    extent: Optional[Tuple[int, int]],
    signature: str,
    configuration_digit: str,
    fidelity: Optional[str] = None,
) -> bytes:
    attributes: dict[str, object] = {
        "line": 2,
        "scope": "function",
        "signature": signature,
    }
    if extent is not None:
        attributes["extent_start"] = extent[0]
        attributes["extent_end"] = extent[1]
        if fidelity is not None:
            attributes["extent_fidelity"] = fidelity
    return json.dumps(
        {
            "artifact": "archbird-provider-facts",
            "capabilities": [
                {
                    "claims": ["syntax-structure"],
                    "coverage": "complete",
                    "domain": "symbols",
                }
            ],
            "diagnostics": [],
            "facts": [
                {
                    "attributes": attributes,
                    "claim": "syntax-structure",
                    "correlation": "span",
                    "domain": "symbols",
                    "id": f"symbol:{name}",
                    "key": name,
                    "kind": "function",
                    "name": name,
                    "path": "case.py",
                    "project": project.project,
                    "span": {"end": anchor[1], "start": anchor[0]},
                }
            ],
            "inputs": [
                {
                    "project": project.project,
                    "source_manifest_sha256": project.manifest_sha256,
                }
            ],
            "producer": {
                "configuration_sha256": configuration_digit * 64,
                "implementation_sha256": "f" * 64,
                "name": producer,
                "version": "1",
            },
            "provenance": "derived",
            "resolutions": [],
            "schema_version": 1,
            "subject": {
                "path": "case.py",
                "project": project.project,
                "scope": "file",
            },
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()


def symbol_extents(project: Project) -> Mapping[str, Mapping[str, int]]:
    files = project.file_facts()["files"]
    if len(files) != 1:
        raise AssertionError(f"expected one file-facts row, received {len(files)}")
    return {
        symbol["name"]: symbol["extent"]
        for symbol in files[0]["symbols"]
        if "extent" in symbol
    }


def symbol_spans(project: Project) -> Mapping[str, Mapping[str, int]]:
    files = project.file_facts()["files"]
    if len(files) != 1:
        raise AssertionError(f"expected one file-facts row, received {len(files)}")
    return {
        symbol["name"]: symbol["span"]
        for symbol in files[0]["symbols"]
        if "span" in symbol
    }


def test_real_python_providers() -> None:
    raw = (
        b"@decorate\n"
        b"def decorated():\n"
        b"    return 1\n"
        b"    # concrete-syntax trailing comment\n"
        b"\n"
        b"def outer():\n"
        b"    def inner():\n"
        b"        return 2\n"
        b"        # nested trailing comment\n"
        b"    return inner\n"
        b"    # outer trailing comment\n"
        b"\n"
        b"class Wrapper:\n"
        b"    def method(self):\n"
        b"        return 3\n"
        b"        # method trailing comment\n"
    )
    project = Project(
        "combined-python-extents",
        [Source("case.py", raw, language="python", layer="python")],
    )
    project.set_config(
        json.dumps(
            {
                "project": "combined-python-extents",
                "layers": [
                    {
                        "name": "python",
                        "role": "core",
                        "language": "python",
                        "globs": ["**/*.py"],
                    }
                ],
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
    )
    project.scan(map_cache=False)
    ledger = json.loads(project.merge_ledger_json())
    if ledger["summary"]["conflicts"] != 0:
        raise AssertionError(f"valid provider extents conflicted: {ledger!r}")

    providers = {
        row["sha256"]: row["producer"]["name"] for row in ledger["providers"]
    }
    extent_variations = [
        row for row in ledger["variations"] if row["attribute"].startswith("extent_")
    ]
    varied_symbols = {row["witness"]["key"] for row in extent_variations}
    expected = {
        "decorated",
        "outer",
        "outer.inner",
        "Wrapper",
        "Wrapper.method",
    }
    if not expected.issubset(varied_symbols):
        raise AssertionError(
            f"combined providers did not exercise all extent boundaries: "
            f"{expected - varied_symbols!r}"
        )
    for variation in extent_variations:
        if (
            providers[variation["canonical_provider"]]
            != "archbird-tree-sitter-python"
            or providers[variation["alternate_provider"]]
            != "archbird-python-ast"
        ):
            raise AssertionError(
                f"provider precedence or provenance changed: {variation!r}"
            )

    extents = symbol_extents(project)
    spans = symbol_spans(project)
    trailing_marker = {
        "decorated": b"# concrete-syntax trailing comment",
        "outer": b"# outer trailing comment",
        "outer.inner": b"# nested trailing comment",
        "Wrapper": b"# method trailing comment",
        "Wrapper.method": b"# method trailing comment",
    }
    for name in expected:
        extent = extents[name]
        selected = raw[extent["start"] : extent["end"]]
        if trailing_marker[name] not in selected:
            raise AssertionError(
                f"{name} did not retain the concrete-syntax boundary"
            )
        anchor = spans[name]
        leaf = name.rsplit(".", 1)[-1].encode()
        if raw[anchor["start"] : anchor["end"]] != leaf:
            raise AssertionError(
                f"{name} did not retain its exact declaration anchor"
            )
        symbol = next(
            row
            for row in project.file_facts()["files"][0]["symbols"]
            if row["name"] == name
        )
        if not symbol["fact_id"].startswith("f:"):
            raise AssertionError(f"{name} omitted its canonical fact identity")
    decorated = extents["decorated"]
    if not raw[decorated["start"] : decorated["end"]].startswith(
        b"@decorate\ndef decorated"
    ):
        raise AssertionError("decorated canonical extent omitted its wrapper")
    source = project.source_markdown(
        artifact_json=query_map_json(
            project.map_json(),
            symbols=("case.py:decorated",),
            depth=0,
            test_depth=0,
        )
    ).decode()
    if (
        "# concrete-syntax trailing comment" not in source
        or "def outer" in source
    ):
        raise AssertionError(
            "standard source view did not use the concrete canonical extent"
        )


def test_precedence_and_variation_capacity() -> None:
    raw = (
        b"@decorate\n"
        b"def target():\n"
        b"    return 1\n"
        b"    # alternate trailing comment\n"
    )
    anchor_start = raw.index(b"target")
    anchor = (anchor_start, anchor_start + len(b"target"))
    primary_end = raw.index(b"\n", raw.index(b"return 1")) + 1
    alternate_start = raw.index(b"def target")
    project = Project(
        "synthetic-extent-precedence",
        [Source("case.py", raw, language="python", layer="python")],
    )
    project.add_provider(
        provider_bundle(
            project,
            producer="fixture-primary",
            name="target",
            anchor=anchor,
            extent=(0, primary_end),
            signature="def target()",
            configuration_digit="1",
            fidelity="semantic",
        ),
        "primary",
    )
    project.add_provider(
        provider_bundle(
            project,
            producer="fixture-augment",
            name="target",
            anchor=anchor,
            extent=(alternate_start, len(raw)),
            signature="target()",
            configuration_digit="2",
            fidelity="concrete",
        ),
        "augment",
    )
    project.finalize_providers()
    ledger = json.loads(project.merge_ledger_json())
    if ledger["summary"]["conflicts"] != 0:
        raise AssertionError("compatible synthetic extents conflicted")
    attributes = {row["attribute"] for row in ledger["variations"]}
    if attributes != {
        "extent_end",
        "extent_start",
        "signature",
    }:
        raise AssertionError(
            f"alternate extent provenance was not complete: {attributes!r}"
        )
    if symbol_extents(project)["target"] != {
        "start": alternate_start,
        "end": len(raw),
    }:
        raise AssertionError(
            "semantic provider replaced the concrete canonical extent"
        )


def test_invalid_extents_remain_fatal() -> None:
    raw = b"def target():\n    return 1\n"
    anchor_start = raw.index(b"target")
    anchor = (anchor_start, anchor_start + len(b"target"))
    invalid = {
        "missing-end": (None, "semantic"),
        "reversed": ((anchor_start, anchor_start), "semantic"),
        "misses-anchor": ((anchor[1] + 1, len(raw)), "semantic"),
        "outside-source": ((0, len(raw) + 1), "semantic"),
        "unknown-fidelity": ((0, len(raw)), "approximate"),
    }
    for index, (case, (extent, fidelity)) in enumerate(invalid.items(), 1):
        project = Project(
            f"invalid-extent-{case}",
            [Source("case.py", raw, language="python", layer="python")],
        )
        bundle = json.loads(
            provider_bundle(
                project,
                producer=f"fixture-{case}",
                name="target",
                anchor=anchor,
                extent=extent,
                signature="def target()",
                configuration_digit=str(index),
                fidelity=fidelity,
            )
        )
        if case == "missing-end":
            bundle["facts"][0]["attributes"]["extent_start"] = 0
        try:
            project.add_provider(
                json.dumps(
                    bundle, sort_keys=True, separators=(",", ":")
                ).encode(),
                "primary",
            )
        except RuntimeError as error:
            message = str(error)
            if f"fixture-{case}" not in message or "case.py" not in message:
                raise AssertionError(
                    f"invalid extent diagnostic lacks provider/path: {message}"
                ) from error
        else:
            raise AssertionError(f"{case} declaration extent was accepted")

    project = Project(
        "disjoint-provider-extents",
        [Source("case.py", raw, language="python", layer="python")],
    )
    project.add_provider(
        provider_bundle(
            project,
            producer="fixture-valid-primary",
            name="target",
            anchor=anchor,
            extent=(0, len(raw)),
            signature="def target()",
            configuration_digit="5",
            fidelity="concrete",
        ),
        "primary",
    )
    try:
        project.add_provider(
            provider_bundle(
                project,
                producer="fixture-disjoint-augment",
                name="target",
                anchor=anchor,
                extent=(anchor[1] + 1, len(raw)),
                signature="def target()",
                configuration_digit="6",
                fidelity="concrete",
            ),
            "augment",
        )
    except RuntimeError as error:
        message = str(error)
        if "fixture-disjoint-augment" not in message or "case.py" not in message:
            raise AssertionError(
                f"disjoint extent diagnostic lacks provider/path: {message}"
            ) from error
    else:
        raise AssertionError("disjoint augment declaration extent was accepted")


def test_conflict_diagnostic_names_witnesses() -> None:
    raw = b"def target():\n    return 1\n"
    anchor_start = raw.index(b"target")
    anchor = (anchor_start, anchor_start + len(b"target"))
    project = Project(
        "diagnostic-extent-conflict",
        [Source("case.py", raw, language="python", layer="python")],
    )
    for mode, producer, name, digit in (
        ("primary", "fixture-primary", "target", "1"),
        ("augment", "fixture-augment", "other", "2"),
    ):
        project.add_provider(
            provider_bundle(
                project,
                producer=producer,
                name=name,
                anchor=anchor,
                extent=(0, len(raw)),
                signature=f"def {name}()",
                configuration_digit=digit,
                fidelity="concrete",
            ),
            mode,
        )
    try:
        project.finalize_providers()
    except RuntimeError as error:
        message = str(error)
        for expected in (
            "augment-mismatch",
            "case.py",
            "fixture-primary",
            "fixture-augment",
        ):
            if expected not in message:
                raise AssertionError(
                    f"merge conflict diagnostic omitted {expected!r}: {message}"
                ) from error
    else:
        raise AssertionError("incompatible declaration identities finalized")


def main() -> int:
    test_real_python_providers()
    test_precedence_and_variation_capacity()
    test_invalid_extents_remain_fatal()
    test_conflict_diagnostic_names_witnesses()
    print(
        "source extent merge passed: combined providers, precedence, "
        "invalid ranges, diagnostics"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
