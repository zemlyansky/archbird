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
        is_c_source = path.endswith((".c", ".h"))
        row = {
            "bytes": len(data),
            "path": path,
            "roles": ["source"] if is_c_source else ["build", "index"],
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        if is_c_source:
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
    extension.project_scan_builtin_provider(
        project, "syntax:tree-sitter:c", "primary"
    )
    extension.project_finalize_providers(project)
    return json.loads(extension.project_map(project))


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_compile_commands.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    debug = [
        {
            "arguments": [
                "/usr/bin/cc",
                "-g",
                "-iquote../../quotes",
                "-I",
                "../../include",
                "-I../../variants/debug",
                "-isystem",
                "../../system",
                "-I/usr/include",
                "-c",
                "/checkout/src/a.c",
            ],
            "directory": "/checkout/build/debug",
            "file": "/checkout/src/a.c",
        },
        {
            "command": (
                "'/opt/toolchains/clang' -g -iquote \"quote headers\" "
                "-Iinclude -isystem system -c src/b.c"
            ),
            "directory": "/checkout",
            "file": "src/b.c",
        },
        {
            "command": "cc -c src/c.c",
            "directory": "/checkout",
            "file": "src/c.c",
        },
        {
            "arguments": [
                "cc",
                "-Ifirst",
                "-Isecond",
                "-c",
                "src/order.c",
            ],
            "directory": "/checkout",
            "file": "src/order.c",
        },
        {
            "command": "cc -c generated.c",
            "directory": "/checkout",
            "file": "generated.c",
        },
        {
            "command": "'cc -Iinclude -c src/a.c",
            "directory": "/checkout",
            "file": "src/a.c",
        },
    ]
    release = [
        {
            "command": (
                "/usr/local/bin/cc -O3 -iquote ../../quotes "
                "-I../../include -I../../variants/release "
                "-isystem../../system -c /checkout/src/a.c"
            ),
            "directory": "/checkout/build/release",
            "file": "/checkout/src/a.c",
        },
        {
            "arguments": [
                "clang-cl",
                "/O2",
                "/external:Isystem",
                "/Iinclude",
                "-iquotequote headers",
                "/c",
                "src/b.c",
            ],
            "directory": "C:\\checkout",
            "file": "src/b.c",
        },
        {
            "arguments": [
                "cc",
                "-Isecond",
                "-Ifirst",
                "-c",
                "src/order.c",
            ],
            "directory": "/checkout",
            "file": "src/order.c",
        },
    ]
    sources = {
        "build/debug/compile_commands.json": canonical(debug),
        "build/release/compile_commands.json": canonical(release),
        "include/shared.h": b"#define SHARED 1\n",
        "include/orphan.h": b"#include <shared.h>\n",
        "include/wrapper.h": b"#include <system.h>\n",
        "first/choice.h": b"#define CHOICE 1\n",
        "quote headers/quoted path.h": b"#define QUOTED 1\n",
        "quotes/quote_a.h": b"#define QUOTE_A 1\n",
        "second/choice.h": b"#define CHOICE 2\n",
        "src/a.c": (
            b'#include "quote_a.h"\n'
            b"#include <shared.h>\n"
            b"#include <wrapper.h>\n"
            b"#include <system.h>\n"
            b"#include <variant.h>\n"
            b"int a(void) { return SHARED + QUOTE_A + SYSTEM; }\n"
        ),
        "src/b.c": (
            b'#include "quoted path.h"\n'
            b"#include <shared.h>\n"
            b"#include <wrapper.h>\n"
            b"int b(void) { return SHARED + QUOTED; }\n"
        ),
        "src/c.c": b"int c(void) { return 0; }\n",
        "src/order.c": (
            b"#include <choice.h>\n"
            b"int order(void) { return CHOICE; }\n"
        ),
        "system/system.h": b"#define SYSTEM 1\n",
        "variants/debug/variant.h": b"#define VARIANT 1\n",
        "variants/release/variant.h": b"#define VARIANT 2\n",
    }
    config = {
        "project": "compile-commands",
        "layers": [
            {
                "name": "core",
                "role": "core",
                "language": "c",
                "globs": [
                    "include/**/*.h",
                    "first/**/*.h",
                    "quote headers/**/*.h",
                    "quotes/**/*.h",
                    "second/**/*.h",
                    "src/**/*.c",
                    "system/**/*.h",
                    "variants/**/*.h",
                ],
                "import_roots": ["include"],
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
    assert len(routes) == 7, routes
    assert [(row["variant"], row["name"]) for row in routes] == [
        ("debug", "src/a.c"),
        ("debug", "src/b.c"),
        ("debug", "src/c.c"),
        ("debug", "src/order.c"),
        ("release", "src/a.c"),
        ("release", "src/b.c"),
        ("release", "src/order.c"),
    ]
    assert [row["command"] for row in routes] == [
        "cc",
        "clang",
        "cc",
        "cc",
        "cc",
        "clang-cl",
        "cc",
    ]
    assert all(row["paths"] == [row["name"]] for row in routes)
    assert [row["conditions"][0] for row in routes] == [
        "compile-source:suffix",
        "compile-source:exact",
        "compile-source:exact",
        "compile-source:exact",
        "compile-source:suffix",
        "compile-source:exact",
        "compile-source:exact",
    ], routes
    assert all(
        len(row["conditions"]) == 2
        and row["conditions"][1].startswith("command-sha256:")
        and len(row["conditions"][1]) == 31
        for row in routes
    )
    encoded_routes = canonical(routes)
    encoded_map = canonical(mapped)
    assert b"/checkout" not in encoded_routes
    assert b"/usr/" not in encoded_routes
    assert b"/checkout" not in encoded_map
    assert b"C:\\\\checkout" not in encoded_map
    assert [row["code"] for row in mapped["diagnostics"]] == [
        "compile-command-invalid",
        "compile-command-unmapped",
        "unresolved-import",
        "unresolved-import",
    ], mapped["diagnostics"]
    local_imports = {
        (row["source"], row["target"])
        for row in mapped["edges"]
        if row["kind"] == "import" and row["target"] != "unresolved-import"
    }
    assert local_imports == {
        ("src/a.c", "include/shared.h"),
        ("src/a.c", "include/wrapper.h"),
        ("src/a.c", "quotes/quote_a.h"),
        ("src/a.c", "system/system.h"),
        ("src/b.c", "include/shared.h"),
        ("src/b.c", "include/wrapper.h"),
        ("src/b.c", "quote headers/quoted path.h"),
        ("include/orphan.h", "include/shared.h"),
        ("include/wrapper.h", "system/system.h"),
    }, mapped["edges"]
    assert not any(
        row["target"].endswith("/variant.h")
        for row in mapped["edges"]
        if row["kind"] == "import"
    )
    assert any(
        row["source"] == "src/a.c"
        and row["target"] == "unresolved-import"
        and row["names"] == ["variant.h"]
        for row in mapped["edges"]
    )
    assert any(
        row["source"] == "src/order.c"
        and row["target"] == "unresolved-import"
        and row["names"] == ["choice.h"]
        for row in mapped["edges"]
    )
    compile_edges = [
        row
        for row in mapped["edges"]
        if row["kind"] == "import"
        and row["source"] in {"include/wrapper.h", "src/a.c", "src/b.c"}
        and row["target"] != "unresolved-import"
    ]
    assert all(
        row["evidence"]
        == [
            {
                "basis": "compile-command-search-path",
                "provider": "build/debug/compile_commands.json",
                "state": "current",
            },
            {
                "basis": "compile-command-search-path",
                "provider": "build/release/compile_commands.json",
                "state": "current",
            },
        ]
        for row in compile_edges
    ), compile_edges
    orphan_edge = next(
        row
        for row in mapped["edges"]
        if row["source"] == "include/orphan.h"
        and row["target"] == "include/shared.h"
    )
    assert "evidence" not in orphan_edge, orphan_edge

    matching_commands = [
        {
            "arguments": ["cc", "-c", "/checkout/deep/raw.c"],
            "directory": "/checkout",
            "file": "/checkout/deep/raw.c",
        },
        {
            "arguments": ["clang-cl", "/c", "src\\windows.c"],
            "directory": "C:\\checkout\\nested",
            "file": "src\\windows.c",
        },
    ]
    matching_sources = {
        "compile_commands.json": canonical(matching_commands),
        "deep/raw.c": b"int raw(void) { return 0; }\n",
        "nested/src/windows.c": b"int windows(void) { return 0; }\n",
        "src/windows.c": b"int fallback(void) { return 0; }\n",
    }
    matching_config = {
        "project": "compile-commands",
        "layers": [
            {
                "name": "core",
                "role": "core",
                "language": "c",
                "globs": ["**/*.c"],
            }
        ],
        "builds": [
            {
                "kind": "compile_commands",
                "name": "commands",
                "path": "compile_commands.json",
                "variant": "default",
            }
        ],
    }
    matching = build_map(extension, matching_sources, matching_config)
    assert [
        (row["name"], row["conditions"][0]) for row in matching["builds"]
    ] == [
        ("deep/raw.c", "compile-source:suffix"),
        ("nested/src/windows.c", "compile-source:directory-suffix"),
    ], matching["builds"]

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
