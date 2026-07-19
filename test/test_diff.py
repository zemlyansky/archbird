#!/usr/bin/env python3
"""Differential structural map-diff tests for the native C kernel."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.analyze import analyze
from archbird.map.config import load_config
from archbird.map.diff import diff_map_documents
from archbird.map.render import _diff_dict, _map_dict


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def changed_map(before: dict) -> dict:
    after = copy.deepcopy(before)
    after["evidence"]["input_sha256"] = "f" * 64
    after["tool"]["version"] = "99.0.0"
    after["files"][0]["sha256"] = "e" * 64
    after["files"][0]["symbols"][0]["signature"] += " changed"
    after["edges"][0]["names"].append("new-edge-name")
    after["call_resolutions"][0]["count"] += 1
    after["packages"][0]["exports"].append("new_export")
    first_origin = next(iter(after["packages"][0]["export_origins"]))
    after["packages"][0]["export_origins"][first_origin].append("new/path.js")
    after["packages"][0]["dependencies"][0]["requirement"] = ">=99"
    first_entry = next(iter(after["packages"][0]["entrypoints"]))
    after["packages"][0]["entrypoints"][first_entry] = "new/entry.js"
    after["tests"][0]["routes"]["new/test/target.c"] = 1
    if after["components"]:
        after["components"][0]["files"].append("new/component.c")
        first_route = next(iter(after["components"][0]["outgoing"]))
        after["components"][0]["outgoing"][first_route].append("new-route")
    else:
        after["components"].append(
            {
                "description": "new",
                "files": ["new/component.c"],
                "name": "new-component",
                "outgoing": {},
                "symbol_count": 0,
            }
        )
    if after["parity"]:
        after["parity"][0]["members"][0]["missing"].append("new-gap")
    if after["surfaces"]:
        after["surfaces"][0]["names"][0]["candidates"].append("new/candidate.c")
    after["artifacts"][0]["depends_on"].append("new-artifact")
    after["builds"][0]["paths"].append("new/build/input")
    return after


def compare(extension, before: dict, after: dict) -> None:
    native = json.loads(extension.map_diff(canonical(before), canonical(after)))
    expected = _diff_dict(diff_map_documents(before, after))
    native.pop("tool")
    expected.pop("tool")
    # The immutable Python reference predates schema-7 symbol and entrypoint
    # relations. Keep it as the oracle for the shared legacy sections; native
    # schema-7 coverage is asserted independently below.
    native["schema_version"] = expected["schema_version"]
    for native_only in (
        "package_entrypoint_surfaces",
        "symbol_calls",
        "symbol_references",
        "test_route_evidence",
    ):
        native["sections"].pop(native_only)
    if native != expected:
        raise AssertionError(
            "native diff differs\n"
            + json.dumps(
                {"native": native, "expected": expected},
                indent=2,
                sort_keys=True,
            )
        )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_diff.py EXTENSION FIXTURE")
    extension = load_extension(Path(sys.argv[1]).resolve())
    fixture = Path(sys.argv[2]).resolve()
    data = analyze(load_config(fixture / "archbird.json", root_override=fixture))
    before = _map_dict(data)
    compare(extension, before, before)
    duplicate_name = copy.deepcopy(before)
    call = copy.deepcopy(duplicate_name["call_resolutions"][0])
    call["candidates"] = []
    call["kind"] = "method"
    duplicate_name["call_resolutions"].append(call)
    duplicate_name["call_resolutions"].sort(
        key=lambda row: (
            row["source"],
            row["name"],
            row["kind"],
            row["count"],
            row["candidates"],
        )
    )
    compare(extension, duplicate_name, duplicate_name)
    compare(extension, before, duplicate_name)
    after = changed_map(before)
    compare(extension, before, after)
    first = extension.map_diff(canonical(before), canonical(after))
    second = extension.map_diff(canonical(before), canonical(after))
    if first != second:
        raise AssertionError("native diff bytes are not repeatable")
    schema7_before = copy.deepcopy(before)
    schema7_before["schema_version"] = 7
    schema7_before["symbol_calls"] = [
        {
            "candidates": [
                {"path": "src/target.c", "symbol": "target"}
            ],
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "call-1",
                    "line": 4,
                    "provider": "fixture",
                    "span": {"end": 20, "start": 14},
                }
            ],
            "name": "target",
            "resolution": "candidate",
            "source": {"path": "src/source.c", "symbol": "source"},
        },
        {
            "candidates": [
                {"path": "src/target.c", "symbol": "target"}
            ],
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "call-file-1",
                    "line": 12,
                    "provider": "fixture",
                    "span": {"end": 72, "start": 66},
                }
            ],
            "name": "target",
            "resolution": "candidate",
            "source": {"path": "test.js", "scope": "test-file"},
        },
    ]
    schema7_before["symbol_references"] = [
        {
            "candidates": [
                {"path": "src/target.c", "symbol": "target"}
            ],
            "container": "routes",
            "context": "initializer",
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "reference-1",
                    "line": 8,
                    "provider": "fixture",
                    "span": {"end": 50, "start": 44},
                }
            ],
            "name": "target",
            "relation": "value",
            "resolution": "candidate",
            "source": {"path": "src/source.c", "symbol": "source"},
        },
        {
            "candidates": [
                {"path": "src/target.c", "symbol": "target"}
            ],
            "context": "identifier",
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "reference-file-1",
                    "line": 14,
                    "provider": "fixture",
                    "span": {"end": 88, "start": 82},
                }
            ],
            "name": "target",
            "relation": "value",
            "resolution": "candidate",
            "source": {"path": "index.js", "scope": "file"},
        },
    ]
    schema7_before["packages"][0]["entrypoint_surfaces"] = [
        {
            "export_origins": {"target": ["src/target.c"]},
            "exports": ["target"],
            "partial": False,
            "path": "src/index.js",
        }
    ]
    route_location = (0, 0)
    schema7_before["tests"][0]["cases"][0]["route_evidence"] = [
        {
            "claim": "syntax-reference",
            "enclosing": "",
            "fact_id": "route-1",
            "line": 4,
            "name": "target",
            "provenance": "derived",
            "provider": "fixture",
            "relation": "call",
            "scope": "case",
            "span": {"end": 24, "start": 18},
            "target": "src/target.c",
            "target_symbol": "target",
        }
    ]
    reordered_before = copy.deepcopy(schema7_before)
    for section in ("symbol_calls", "symbol_references"):
        reordered_before[section][0]["candidates"].append(
            {"path": "src/alternate.c", "symbol": "alternate"}
        )
        reordered_before[section][0]["evidence"].append(
            {
                "claim": "lexical-occurrence",
                "fact_id": f"{section}-alternate",
                "line": 3,
                "provider": "fixture-lexical",
                "span": {"end": 13, "start": 7},
            }
        )
    reordered_after = copy.deepcopy(reordered_before)
    for section in ("symbol_calls", "symbol_references"):
        reordered_after[section][0]["candidates"].reverse()
        reordered_after[section][0]["evidence"].reverse()
    reordered = json.loads(
        extension.map_diff(canonical(reordered_before), canonical(reordered_after))
    )
    for section in ("symbol_calls", "symbol_references"):
        if any(reordered["sections"][section].values()):
            raise AssertionError({section: reordered["sections"][section]})
    schema7_after = copy.deepcopy(schema7_before)
    schema7_after["symbol_calls"][0]["evidence"][0]["line"] = 5
    schema7_after["symbol_references"][0]["resolution"] = "unique"
    schema7_after["packages"][0]["entrypoint_surfaces"][0]["exports"].append(
        "extra"
    )
    schema7_after["tests"][route_location[0]]["cases"][route_location[1]][
        "route_evidence"
    ][0]["line"] += 1
    schema7 = json.loads(
        extension.map_diff(canonical(schema7_before), canonical(schema7_after))
    )
    if schema7["schema_version"] != 7:
        raise AssertionError(schema7["schema_version"])
    for section in (
        "package_entrypoint_surfaces",
        "symbol_calls",
        "symbol_references",
        "test_route_evidence",
    ):
        change = schema7["sections"][section]
        if len(change["changed"]) != 1 or change["added"] or change["removed"]:
            raise AssertionError({section: change})
    print(
        "native saved-map diff parity passed: legacy sections and schema-7 relations"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
