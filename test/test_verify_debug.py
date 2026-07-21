#!/usr/bin/env python3
"""Selection-accounting and verification-debug contracts."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from archbird.native import Verification


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load extension {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def encoded(document: object) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode()


def map_document(*, discovery: bool = True) -> dict:
    result = {
        "artifact": "map",
        "schema_version": 7,
        "project": "debug-test",
        "evidence": {"config_sha256": "1" * 64, "input_sha256": "2" * 64},
        "tool": {
            "name": "archbird",
            "version": "fixture",
            "implementation_sha256": "3" * 64,
        },
        "diagnostics": [],
        "files": [
            {
                "bytes": size,
                "language": "c",
                "layer": "core",
                "path": path,
                "sha256": digit * 64,
            }
            for path, size, digit in (
                ("src/a.c", 5, "4"),
                ("src/b.c", 8, "5"),
                ("test/generated.c", 13, "6"),
            )
        ],
    }
    if discovery:
        result["discovery"] = {
            "coverage": {
                "assets": 2,
                "ignored": 3,
                "inventory_files": 12,
                "oversized": 0,
                "pruned_directories": 0,
                "selected": 7,
                "unsupported_known": 4,
            },
            "sha256": "7" * 64,
        }
    return result


def suite_document() -> dict:
    return {
        "schema_version": 1,
        "suite": "debug-suite",
        "projects": {"subject": {"map": "ARCHBIRD.json"}},
        "extractors": {
            "expected": {"kind": "literal_values", "values": {"A": 1}},
            "actual": {"kind": "literal_values", "values": {"A": 1}},
            "source.bytes": {
                "kind": "file_metrics",
                "metric": "bytes",
                "project": "subject",
                "include": ["src/**"],
            },
        },
        "checks": [
            {
                "id": "VALUES",
                "assert": "values_equal",
                "actual": "actual",
                "expected": "expected",
                "owner": "test",
                "rationale": "Exercise check-to-extractor selection.",
            },
            {
                "id": "FILE-SIZE",
                "assert": "numeric_bounds",
                "actual": "source.bytes",
                "max": 10,
                "owner": "test",
                "rationale": "Exercise exact file selection accounting.",
            },
        ],
    }


def input_document(*, discovery: bool = True) -> dict:
    return {
        "artifact": "verification-input",
        "schema_version": 1,
        "suite_path": "archbird.verify.json",
        "projects": [
            {
                "name": "subject",
                "map": map_document(discovery=discovery),
                "sources": [],
            }
        ],
        "provided_facts": [],
        "attestations": [],
        "baseline": None,
    }


def request(
    view: str, *, check: str | None = None, extractor: str | None = None
) -> bytes:
    result = {
        "artifact": "verification-debug-request",
        "schema_version": 1,
        "view": view,
    }
    if check is not None:
        result["check"] = check
    if extractor is not None:
        result["extractor"] = extractor
    return encoded(result)


def debug(extension, suite: bytes, input_json: bytes, request_json: bytes) -> dict:
    return json.loads(
        extension.verification_debug(
            suite, input_json, request_json, "json", False
        )
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_verify_debug.py EXTENSION REPOSITORY")
    extension = load_extension(Path(sys.argv[1]).resolve())
    repository = Path(sys.argv[2]).resolve()
    suite = encoded(suite_document())
    current_input = encoded(input_document())

    selected = debug(extension, suite, current_input, request("selection"))
    rows = {row["extractor"]: row for row in selected["selections"]}
    if selected["artifact"] != "verification-debug" or set(rows) != {
        "actual",
        "expected",
        "source.bytes",
    }:
        raise AssertionError(f"unexpected selection debug artifact: {selected!r}")
    metric = rows["source.bytes"]
    if (
        metric["classification"] != "complete"
        or metric["selection_complete"] is not True
        or metric["enumeration_complete"] is not True
        or metric["unit"] != "mapped_source_file"
        or metric["counts"]
        != {
            "evaluated": 2,
            "excluded": 1,
            "selected": 2,
            "unknown": 0,
            "universe": 3,
            "unsupported": 0,
        }
        or metric["truncated"] is not False
    ):
        raise AssertionError(f"file selection accounting is not exact: {metric!r}")
    project = selected["projects"][0]
    if (
        project["state"] != "complete"
        or project["mapped_files"] != 3
        or project["coverage"]["unsupported_known"] != 4
    ):
        raise AssertionError(f"repository coverage context was lost: {project!r}")

    by_check = debug(
        extension, suite, current_input, request("selection", check="VALUES")
    )
    if [row["extractor"] for row in by_check["selections"]] != [
        "actual",
        "expected",
    ] or [row["id"] for row in by_check["checks"]] != ["VALUES"]:
        raise AssertionError(f"check selection filter drifted: {by_check!r}")
    by_extractor = debug(
        extension,
        suite,
        current_input,
        request("selection", extractor="source.bytes"),
    )
    if [row["id"] for row in by_extractor["checks"]] != ["FILE-SIZE"]:
        raise AssertionError(f"extractor filter lost its check: {by_extractor!r}")

    ordinary = json.loads(extension.verification_analyze(suite, current_input, False))
    if any("selection" in fact for fact in ordinary["facts"]):
        raise AssertionError("debug accounting changed the canonical fact schema")

    old_input = encoded(input_document(discovery=False))
    unresolved = debug(extension, suite, old_input, request("unknown"))
    unresolved_rows = {
        row["extractor"]: row for row in unresolved["selections"]
    }
    scopes = {row["scope"] for row in unresolved["unknowns"]}
    if (
        set(unresolved_rows) != {"source.bytes"}
        or unresolved_rows["source.bytes"]["classification"] != "unknown"
        or unresolved_rows["source.bytes"]["selection_complete"] is not None
        or unresolved_rows["source.bytes"]["enumeration_complete"] is not None
        or not {"fact", "fact-item", "check", "finding"}.issubset(scopes)
    ):
        raise AssertionError(f"unknown evidence is not localized: {unresolved!r}")

    diagnostic_input = input_document()
    diagnostic_input["projects"][0]["map"]["diagnostics"] = [
        {
            "code": "fixture-map-error",
            "message": "fixture project map is incomplete",
            "path": "src/a.c",
            "severity": "error",
            "span": {"start": 0, "end": 1},
        }
    ]
    diagnostic_debug = debug(
        extension, suite, encoded(diagnostic_input), request("unknown")
    )
    if diagnostic_debug["diagnostics"] != [
        {
            "code": "project-map-errors",
            "message": "project subject map has validation errors",
            "path": "",
            "severity": "error",
        }
    ] or diagnostic_debug["summary"]["diagnostics"] != 1:
        raise AssertionError(
            f"verification diagnostics were not preserved: {diagnostic_debug!r}"
        )

    attestation_suite = suite_document()
    attestation_suite["attestations"] = {
        "reference.behavior": {
            "project": "subject",
            "path": "reference.attestation.json",
        },
        "subject.behavior": {
            "project": "subject",
            "path": "subject.attestation.json",
        },
    }
    attestation_suite["checks"].append(
        {
            "id": "BEHAVIOR",
            "assert": "attestation_equal",
            "expected": "reference.behavior",
            "actual": "subject.behavior",
            "owner": "test",
            "rationale": "Exercise unavailable attestation localization.",
        }
    )
    attestation_input = input_document()
    attestation_input["attestations"] = [
        {"name": name, "path": path, "error": "fixture unavailable"}
        for name, path in (
            ("reference.behavior", "reference.attestation.json"),
            ("subject.behavior", "subject.attestation.json"),
        )
    ]
    attestation_unknown = debug(
        extension,
        encoded(attestation_suite),
        encoded(attestation_input),
        request("unknown", check="BEHAVIOR"),
    )
    attestation_rows = [
        row
        for row in attestation_unknown["unknowns"]
        if row["scope"] == "attestation"
    ]
    if (
        {row["key"] for row in attestation_rows}
        != {"reference.behavior", "subject.behavior"}
        or {row["state"] for row in attestation_rows} != {"unknown"}
        or {row["check"] for row in attestation_rows} != {"BEHAVIOR"}
    ):
        raise AssertionError(
            f"unavailable attestations were not localized: {attestation_unknown!r}"
        )
    attestation_markdown = extension.verification_debug(
        encoded(attestation_suite),
        encoded(attestation_input),
        request("unknown", check="BEHAVIOR"),
        "markdown",
        False,
    )
    if (
        b"## Unresolved evidence" not in attestation_markdown
        or b"scope=attestation check=BEHAVIOR" not in attestation_markdown
        or b"key=subject.behavior" not in attestation_markdown
    ):
        raise AssertionError(attestation_markdown.decode(errors="replace"))

    markdown = extension.verification_debug(
        suite,
        current_input,
        request("selection", check="FILE-SIZE"),
        "markdown",
        False,
    )
    if (
        b'Suite `"' in markdown
        or b"source.bytes: classification=complete" not in markdown
        or b"unit=mapped_source_file" not in markdown
    ):
        raise AssertionError(markdown.decode(errors="replace"))

    try:
        extension.verification_debug(
            suite, current_input, request("selection", check="TYPO"), "json", False
        )
    except Exception as error:
        if "check is unknown" not in str(error):
            raise
    else:
        raise AssertionError("debug accepted an unknown check filter")

    with tempfile.TemporaryDirectory(dir=repository / "build") as raw_directory:
        root = Path(raw_directory)
        (root / "archbird.verify.json").write_bytes(suite)
        (root / "ARCHBIRD.json").write_bytes(encoded(map_document()))
        command = [
            str(repository / "archbird"),
            "verify",
            "debug",
            "selection",
            str(root),
            "--check",
            "FILE-SIZE",
            "--format",
            "json",
        ]
        completed = subprocess.run(command, capture_output=True, check=False)
        if completed.returncode:
            raise AssertionError(completed.stderr.decode(errors="replace"))
        cli_document = json.loads(completed.stdout)
        if [row["extractor"] for row in cli_document["selections"]] != [
            "source.bytes"
        ]:
            raise AssertionError(f"CLI selection filter drifted: {cli_document!r}")

    print("verification debug contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
