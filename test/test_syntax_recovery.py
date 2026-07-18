#!/usr/bin/env python3
"""Tree-sitter recovery must weaken only overlapping normalized facts."""

from __future__ import annotations

import hashlib
import json

from archbird import _native
from archbird.native import query_map_json


SOURCE = b"""\
int stable() { return 1; }
int broken() { auto x = ; helper(); }
"""


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def main() -> int:
    path = "src/recovered.cpp"
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(SOURCE),
                "language": "cpp",
                "layer": "core",
                "path": path,
                "roles": ["source"],
                "sha256": hashlib.sha256(SOURCE).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "syntax-recovery-fixture",
            "version": "1",
        },
        "project": "syntax-recovery",
        "schema_version": 1,
    }
    project = _native.project_create(canonical(manifest))
    _native.project_add_source(project, path, SOURCE)
    _native.project_finalize_sources(project)
    _native.project_set_config(
        project,
        canonical(
            {
                "layers": [
                    {
                        "globs": ["src/**/*.cpp"],
                        "language": "cpp",
                        "name": "core",
                    }
                ],
                "project": "syntax-recovery",
                "root": ".",
                "schema_version": 1,
            }
        ),
    )
    _native.project_scan_builtin_provider(
        project, "syntax:tree-sitter:cpp", "primary"
    )
    _native.project_finalize_providers(project)
    mapped_bytes = _native.project_map(project)
    mapped = json.loads(mapped_bytes)
    symbols = {
        row["name"]: row
        for file in mapped["files"]
        for row in file["symbols"]
    }
    if symbols["broken"].get("syntax_recovery") != "intersects":
        raise AssertionError(symbols["broken"])
    if "syntax_recovery" in symbols["stable"]:
        raise AssertionError(symbols["stable"])

    focused = json.loads(
        query_map_json(
            mapped_bytes,
            symbols=[f"{path}:broken"],
            depth=0,
            test_depth=0,
        )
    )
    if focused["matched_symbols"] != [
        {
            "kind": "function",
            "line": 2,
            "name": "broken",
            "path": path,
            "scope": "",
            "syntax_recovery": "intersects",
        }
    ]:
        raise AssertionError(focused["matched_symbols"])
    print("per-fact syntax recovery passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
