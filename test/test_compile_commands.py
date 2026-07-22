#!/usr/bin/env python3
"""Exercise host-neutral compilation-database ingestion and variant identity."""

from __future__ import annotations

import hashlib
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


def build_map(extension, sources: dict[str, bytes], config: dict) -> dict:
    files = []
    for path, data in sorted(sources.items()):
        row = {
            "bytes": len(data),
            "path": path,
            "roles": ["source"] if path.endswith(".c") else ["build", "index"],
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        if path.endswith(".c"):
            row["language"] = "c"
            row["layer"] = "core"
        files.append(row)
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": files,
        "producer": {
            "implementation_sha256": "3" * 64,
            "name": "compile-commands-test-host",
            "version": "1",
        },
        "project": "compile-commands",
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    for path, data in sorted(sources.items()):
        extension.project_add_source(project, path, data)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_scan_builtin_provider(project, "lexical:c", "primary")
    extension.project_finalize_providers(project)
    return json.loads(extension.project_map(project))


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_compile_commands.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    debug = [
        {
            "arguments": ["/usr/bin/cc", "-g", "-c", "/checkout/src/a.c"],
            "directory": "/checkout",
            "file": "/checkout/src/a.c",
        },
        {
            "command": "'/opt/toolchains/clang' -g -c src/b.c",
            "directory": "/checkout",
            "file": "src/b.c",
        },
        {
            "command": "cc -c generated.c",
            "directory": "/checkout",
            "file": "generated.c",
        },
    ]
    release = [
        {
            "command": "/usr/local/bin/cc -O3 -c src/a.c",
            "directory": "/checkout",
            "file": "src/a.c",
        },
        {
            "arguments": ["clang", "-O3", "-c", "src/b.c"],
            "directory": "/checkout",
            "file": "src/b.c",
        },
    ]
    sources = {
        "build/debug/compile_commands.json": canonical(debug),
        "build/release/compile_commands.json": canonical(release),
        "src/a.c": b"int a(void) { return 1; }\n",
        "src/b.c": b"int b(void) { return 2; }\n",
    }
    config = {
        "schema_version": 2,
        "project": "compile-commands",
        "layers": [
            {
                "name": "core",
                "role": "core",
                "language": "c",
                "globs": ["src/**/*.c"],
            }
        ],
        "builds": [
            {
                "kind": "compile_commands",
                "name": "debug-db",
                "path": "build/debug/compile_commands.json",
                "variant": "debug",
            },
            {
                "kind": "compile_commands",
                "name": "release-db",
                "path": "build/release/compile_commands.json",
                "variant": "release",
            },
        ],
    }
    mapped = build_map(extension, sources, config)
    routes = mapped["builds"]
    assert len(routes) == 4, routes
    assert [(row["variant"], row["name"]) for row in routes] == [
        ("debug", "src/a.c"),
        ("debug", "src/b.c"),
        ("release", "src/a.c"),
        ("release", "src/b.c"),
    ]
    assert [row["command"] for row in routes] == ["cc", "clang", "cc", "clang"]
    assert all(row["paths"] == [row["name"]] for row in routes)
    assert [row["conditions"][0] for row in routes] == [
        "compile-source:suffix",
        "compile-source:exact",
        "compile-source:exact",
        "compile-source:exact",
    ]
    assert all(
        len(row["conditions"]) == 2
        and row["conditions"][1].startswith("command-sha256:")
        and len(row["conditions"][1]) == 31
        for row in routes
    )
    encoded_routes = canonical(routes)
    assert b"/checkout" not in encoded_routes
    assert b"/usr/" not in encoded_routes
    assert [row["code"] for row in mapped["diagnostics"]] == [
        "compile-command-unmapped"
    ]

    changed = json.loads(json.dumps(mapped))
    changed["builds"][0]["variant"] = "asan"
    diff = json.loads(extension.map_diff(canonical(mapped), canonical(changed)))
    builds = diff["sections"]["build_routes"]
    assert len(builds["added"]) == 1 and len(builds["removed"]) == 1, builds
    assert builds["changed"] == [], builds

    query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "depth": 1,
                    "direction": "both",
                    "paths": ["src/a.c"],
                    "test_depth": 0,
                }
            ),
        )
    )
    assert [(row["variant"], row["command"]) for row in query["builds"]] == [
        ("debug", "cc"),
        ("release", "cc"),
    ]
    print("compile_commands variants, privacy, Query, and Diff passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
