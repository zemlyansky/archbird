#!/usr/bin/env python3
"""Keep package ownership lookup proportional to mapped package evidence."""

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


def configured_project(extension, package_count: int):
    files: list[dict[str, object]] = []
    sources: list[tuple[str, bytes]] = []
    packages: list[dict[str, object]] = []
    for index in range(package_count):
        root = f"packages/p{index:05d}"
        dependency = f"dependency-{index:05d}"
        manifest_path = f"{root}/package.json"
        source_path = f"{root}/main.js"
        manifest = canonical(
            {
                "dependencies": {dependency: "1.0.0"},
                "name": f"p{index:05d}",
                "version": "1.0.0",
            }
        )
        source = f'import "{dependency}";\n'.encode()
        for path, data, roles, language, layer in (
            (manifest_path, manifest, ["manifest"], None, None),
            (source_path, source, ["source"], "javascript", "javascript"),
        ):
            row: dict[str, object] = {
                "bytes": len(data),
                "path": path,
                "roles": roles,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            if language:
                row["language"] = language
            if layer:
                row["layer"] = layer
            files.append(row)
            sources.append((path, data))
        packages.append(
            {
                "entries": [source_path],
                "kind": "npm",
                "layer": "javascript",
                "name": f"p{index:05d}",
                "path": manifest_path,
            }
        )
    manifest = canonical(
        {
            "artifact": "archbird-source-manifest",
            "files": sorted(files, key=lambda row: str(row["path"]).encode()),
            "producer": {
                "implementation_sha256": "5" * 64,
                "name": "package-scaling-host",
                "version": "1",
            },
            "project": "package-dependency-scaling",
            "schema_version": 1,
        }
    )
    config = canonical(
        {
            "layers": [
                {
                    "globs": ["packages/**/*.js"],
                    "language": "javascript",
                    "name": "javascript",
                }
            ],
            "packages": packages,
            "project": "package-dependency-scaling",
        }
    )
    project = extension.project_create(manifest)
    for path, data in sorted(sources, key=lambda row: row[0].encode()):
        extension.project_add_source(project, path, data)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, config)
    extension.project_scan_builtin_provider(project, "lexical:javascript", "primary")
    extension.project_finalize_providers(project)
    return project


def measure(extension, package_count: int) -> tuple[float, str]:
    project = configured_project(extension, package_count)
    samples: list[float] = []
    digest = ""
    for _ in range(3):
        started = perf_counter()
        result = extension.project_map(project)
        samples.append(perf_counter() - started)
        current_digest = hashlib.sha256(result).hexdigest()
        if digest and current_digest != digest:
            raise AssertionError("package scaling Map is not deterministic")
        digest = current_digest
    document = json.loads(result)
    external = sum(edge["kind"] == "external" for edge in document["edges"])
    if external != package_count or document["diagnostics"]:
        raise AssertionError("package scaling fixture changed meaning")
    return statistics.median(samples), digest


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_package_dependency_scaling.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    large = int(os.environ.get("ARCHBIRD_PACKAGE_SCALING_PACKAGES", "2400"))
    if large < 400 or large % 2:
        raise AssertionError("package scaling count must be even and at least 400")
    small = large // 2
    small_elapsed, _ = measure(extension, small)
    large_elapsed, _ = measure(extension, large)
    if large_elapsed > small_elapsed * 3.3 + 0.05:
        raise AssertionError(
            "package dependency lookup scales quadratically: "
            f"{small}={small_elapsed:.3f}s {large}={large_elapsed:.3f}s"
        )
    print(
        "package dependency scaling passed "
        f"({small}={small_elapsed:.3f}s; {large}={large_elapsed:.3f}s)"
    )


if __name__ == "__main__":
    main()
