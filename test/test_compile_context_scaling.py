#!/usr/bin/env python3
"""Bound equivalent compile-context work by unique contexts, not command rows."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import statistics
import sys
from time import perf_counter


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


def render(extension, command_count: int, file_count: int) -> tuple[bytes, float]:
    if file_count < command_count:
        raise AssertionError("file count must cover every compile command")
    source = b"\n"
    source_sha = hashlib.sha256(source).hexdigest()
    commands = [
        {
            "arguments": [
                "cc",
                "-Iinclude",
                "-c",
                f"/repository/src/unit-{index:05d}.c",
            ],
            "directory": "/repository",
            "file": f"/repository/src/unit-{index:05d}.c",
        }
        for index in range(command_count)
    ]
    database = canonical(commands)
    files: list[dict[str, object]] = [
        {
            "bytes": len(database),
            "path": "compile_commands.json",
            "roles": ["build", "index"],
            "sha256": hashlib.sha256(database).hexdigest(),
        }
    ]
    sources: list[tuple[str, bytes]] = [("compile_commands.json", database)]
    for index in range(file_count):
        if index < command_count:
            path = f"src/unit-{index:05d}.c"
            language = "c"
        else:
            path = f"include/header-{index - command_count:05d}.h"
            language = "c"
        files.append(
            {
                "bytes": len(source),
                "language": language,
                "layer": "c",
                "path": path,
                "roles": ["source"],
                "sha256": source_sha,
            }
        )
        sources.append((path, source))
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": sorted(files, key=lambda row: str(row["path"]).encode()),
        "producer": {
            "implementation_sha256": "4" * 64,
            "name": "compile-context-scaling-host",
            "version": "1",
        },
        "project": "compile-context-scaling",
        "schema_version": 1,
    }
    sources.sort(key=lambda row: row[0].encode())
    config = {
        "project": "compile-context-scaling",
        "layers": [
            {
                "globs": ["include/**/*.h", "src/**/*.c"],
                "language": "c",
                "name": "c",
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
    project = extension.project_create(canonical(manifest))
    for path, data in sources:
        extension.project_add_source(project, path, data)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_finalize_providers(project)
    started = perf_counter()
    mapped = extension.project_map(project)
    elapsed = perf_counter() - started
    document = json.loads(mapped)
    if (
        len(document["files"]) != file_count
        or len(document["builds"]) != command_count
        or document["diagnostics"]
    ):
        raise AssertionError("compile-context scaling fixture changed shape")
    return mapped, elapsed


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_compile_context_scaling.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    large_commands = int(
        os.environ.get("ARCHBIRD_COMPILE_CONTEXT_COMMANDS", "2000")
    )
    large_files = int(
        os.environ.get("ARCHBIRD_COMPILE_CONTEXT_FILES", "4000")
    )
    if large_commands < 2 or large_files < large_commands:
        raise AssertionError("invalid compile-context scaling dimensions")
    small_commands = max(1, large_commands // 2)
    small_files = max(small_commands, large_files // 2)

    durations: dict[str, float] = {}
    for label, command_count, file_count in (
        ("small", small_commands, small_files),
        ("large", large_commands, large_files),
    ):
        samples = []
        outputs = []
        for _ in range(2):
            output, elapsed = render(extension, command_count, file_count)
            outputs.append(output)
            samples.append(elapsed)
        if outputs[0] != outputs[1]:
            raise AssertionError(
                f"{label} compile-context Map is not deterministic"
            )
        durations[label] = statistics.median(samples)

    if (
        durations["large"] > durations["small"] * 3.2 + 0.15
        or durations["large"] >= 8.0
    ):
        raise AssertionError(
            "equivalent compile contexts no longer scale near-linearly: "
            f"{small_commands}x{small_files}={durations['small']:.3f}s "
            f"{large_commands}x{large_files}={durations['large']:.3f}s"
        )
    print(
        "compile-context interning scaling passed "
        f"({small_commands} commands/{small_files} files="
        f"{durations['small']:.3f}s; "
        f"{large_commands} commands/{large_files} files="
        f"{durations['large']:.3f}s)"
    )


if __name__ == "__main__":
    main()
