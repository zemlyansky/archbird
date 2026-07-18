#!/usr/bin/env python3
"""Native saved-Map/Query freshness contract tests."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys


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


def identity(digit: str) -> dict[str, str]:
    return {
        "implementation_sha256": digit,
        "name": "archbird",
        "version": "0.0.1",
    }


def map_document(
    *,
    project: str = "fixture",
    input_sha256: str = "1" * 64,
    config_sha256: str = "2" * 64,
    discovery_sha256: str = "3" * 64,
    tool_sha256: str = "4" * 64,
    files: tuple[tuple[str, str], ...] = (
        ("src/a.c", "a" * 64),
        ("src/b.c", "b" * 64),
    ),
) -> dict[str, object]:
    return {
        "artifact": "map",
        "discovery": {"sha256": discovery_sha256},
        "evidence": {
            "config_sha256": config_sha256,
            "input_sha256": input_sha256,
        },
        "files": [
            {"path": path, "sha256": sha256}
            for path, sha256 in files
        ],
        "project": project,
        "schema_version": 7,
        "tool": identity(tool_sha256),
    }


def query_document(source: dict[str, object]) -> dict[str, object]:
    return {
        "artifact": "query",
        "discovery": copy.deepcopy(source["discovery"]),
        "evidence": copy.deepcopy(source["evidence"]),
        "files": [copy.deepcopy(source["files"][0])],
        "project": source["project"],
        "schema_version": 7,
        "source_tool": copy.deepcopy(source["tool"]),
    }


def expect_error(function, *args) -> None:
    try:
        function(*args)
    except Exception:
        return
    raise AssertionError("invalid freshness input was accepted")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_freshness.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    snapshot = map_document()
    encoded = canonical(snapshot)
    first = extension.map_freshness(encoded, encoded)
    second = extension.map_freshness(encoded, encoded)
    if first != second:
        raise AssertionError("freshness output is not byte-repeatable")
    current = json.loads(first)
    if current["status"] != "current":
        raise AssertionError(current)
    if current["comparisons"] != {
        "configuration": "current",
        "discovery": "current",
        "producer": "current",
        "project": "current",
        "source": "current",
    }:
        raise AssertionError(current["comparisons"])
    if current["files"] != {
        "added": [],
        "changed": [],
        "missing": [],
        "scope": "mapped-inventory",
        "unattributed_input_drift": False,
        "unchanged": 2,
    }:
        raise AssertionError(current["files"])
    if json.loads(extension.map_freshness(encoded, encoded, pretty=True)) != current:
        raise AssertionError("pretty freshness changed artifact semantics")

    configured = copy.deepcopy(snapshot)
    del configured["discovery"]
    configured_encoded = canonical(configured)
    configured_current = json.loads(
        extension.map_freshness(configured_encoded, configured_encoded)
    )
    if configured_current["status"] != "current":
        raise AssertionError(configured_current)
    if configured_current["comparisons"]["discovery"] != "current":
        raise AssertionError(configured_current["comparisons"])

    after = map_document(
        input_sha256="5" * 64,
        files=(
            ("src/a.c", "c" * 64),
            ("src/new.c", "d" * 64),
        ),
    )
    stale = json.loads(extension.map_freshness(encoded, canonical(after)))
    if stale["status"] != "stale":
        raise AssertionError(stale)
    if stale["files"]["added"] != ["src/new.c"]:
        raise AssertionError(stale["files"])
    if stale["files"]["missing"] != ["src/b.c"]:
        raise AssertionError(stale["files"])
    if stale["files"]["changed"] != [
        {
            "after_sha256": "c" * 64,
            "before_sha256": "a" * 64,
            "path": "src/a.c",
        }
    ]:
        raise AssertionError(stale["files"])

    drifted = map_document(
        discovery_sha256="6" * 64,
        tool_sha256="7" * 64,
    )
    context_drift = json.loads(
        extension.map_freshness(encoded, canonical(drifted))
    )
    if context_drift["status"] != "context-drift":
        raise AssertionError(context_drift)

    query = query_document(snapshot)
    unrelated = map_document(
        input_sha256="8" * 64,
        files=(
            ("src/a.c", "a" * 64),
            ("src/b.c", "b" * 64),
            ("src/c.c", "c" * 64),
        ),
    )
    selected = json.loads(
        extension.map_freshness(canonical(query), canonical(unrelated))
    )
    if selected["status"] != "stale":
        raise AssertionError(selected)
    if selected["files"]["scope"] != "selected-snapshot-files":
        raise AssertionError(selected["files"])
    if selected["files"]["added"] or selected["files"]["changed"] or selected["files"]["missing"]:
        raise AssertionError(selected["files"])
    if not selected["files"]["unattributed_input_drift"]:
        raise AssertionError(selected["files"])

    foreign = map_document(project="foreign")
    not_applicable = json.loads(
        extension.map_freshness(encoded, canonical(foreign))
    )
    if not_applicable["status"] != "not-applicable":
        raise AssertionError(not_applicable)

    unknown = copy.deepcopy(snapshot)
    del unknown["discovery"]
    del unknown["evidence"]["config_sha256"]
    del unknown["tool"]
    unknown_result = json.loads(
        extension.map_freshness(canonical(unknown), encoded)
    )
    if unknown_result["status"] != "unknown":
        raise AssertionError(unknown_result)

    malformed = copy.deepcopy(snapshot)
    malformed["evidence"]["input_sha256"] = "not-a-digest"
    expect_error(extension.map_freshness, canonical(malformed), encoded)
    duplicate = copy.deepcopy(snapshot)
    duplicate["files"].append(copy.deepcopy(duplicate["files"][0]))
    expect_error(extension.map_freshness, canonical(duplicate), encoded)
    expect_error(extension.map_freshness, b"{}", encoded)

    print("native saved-artifact freshness contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
