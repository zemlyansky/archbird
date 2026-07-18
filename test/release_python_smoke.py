#!/usr/bin/env python3
"""Clean-install smoke test for the PyPI distribution."""

from __future__ import annotations

import json
from pathlib import Path
import sys

import archbird


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: release_python_smoke.py CONFIG ROOT")
    config = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    if archbird.__version__ != "0.0.1":
        raise AssertionError(f"unexpected version: {archbird.__version__}")
    if archbird.PATTERN_CONTRACT_VERSION != 1:
        raise AssertionError("unexpected configured-pattern contract version")
    if archbird.PATTERN_CONTRACT != "archbird-pcre2-v1":
        raise AssertionError("unexpected configured-pattern contract")
    if archbird.PATTERN_ENGINE != "PCRE2 10.47":
        raise AssertionError("unexpected configured-pattern engine")
    if archbird.PATTERN_UNICODE != "UCD 16.0.0":
        raise AssertionError("unexpected configured-pattern Unicode data")
    if archbird.PATTERN_OPTIONS != (
        "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,"
        "NEVER_CALLOUT,JIT_DISABLED"
    ):
        raise AssertionError("unexpected configured-pattern options")
    project = archbird.Project.from_config(config, root=root)
    if len(project.map_input_sha256) != 64:
        raise AssertionError("installed Python host returned an invalid Map-input digest")
    first = project.map_json()
    if first != project.map_json():
        raise AssertionError("installed Python Map is not deterministic")
    document = json.loads(first)
    if document["artifact"] != "map" or document["project"] != "map-base":
        raise AssertionError("installed Python package returned the wrong map")
    if any(row["severity"] == "error" for row in document["diagnostics"]):
        raise AssertionError(document["diagnostics"])
    freshness = json.loads(archbird.audit_map_freshness(first, first))
    if freshness["status"] != "current":
        raise AssertionError("installed Python freshness audit failed")
    query = project.query(paths=["py/pkg"], depth=0)
    if query["artifact"] != "query" or len(query["files"]) != 2:
        raise AssertionError("installed Python query failed")
    graph = json.loads(project.graph_view_json())
    symbols = json.loads(
        project.graph_view_json(
            view="symbols",
            query={"symbols": ["js/index.js:add"], "depth": 1},
        )
    )
    if graph["artifact"] != "archbird-graph-view" or graph["source"]["artifact"] != "map":
        raise AssertionError("installed Python component graph failed")
    if symbols["request"]["view"] != "symbols" or symbols["source"]["artifact"] != "query":
        raise AssertionError("installed Python symbol graph failed")
    print(
        f"python release smoke passed: files={len(document['files'])} "
        f"symbols={sum(len(row['symbols']) for row in document['files'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
