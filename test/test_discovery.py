#!/usr/bin/env python3
"""Differential test for host inventory -> native discovery classification."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys

from archbird.map.analyze import glob_files
from archbird.map.config import load_config


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def inventory(extension, config_json: bytes, root: Path) -> list[str]:
    """Return canonical regular-file paths without following symlinks."""

    result: list[str] = []
    pending = [root]
    while pending:
        directories: list[tuple[str, Path]] = []
        for directory in pending:
            with os.scandir(directory) as entries:
                for entry in entries:
                    path = Path(entry.path)
                    relative = path.relative_to(root).as_posix()
                    if entry.is_dir(follow_symlinks=False):
                        directories.append((relative, path))
                    elif entry.is_file(follow_symlinks=False):
                        result.append(relative)
        decisions = extension.discovery_descend(
            config_json, [relative for relative, _ in directories]
        )
        pending = [
            path
            for (_, path), should_descend in zip(directories, decisions)
            if should_descend
        ]
    return sorted(result)


def add(
    rows: dict[str, dict[str, object]],
    available: set[str],
    path: str,
    role: str,
    *,
    layer: str = "",
    language: str = "",
) -> None:
    if path not in available:
        return
    row = rows.setdefault(
        path, {"language": "", "layer": "", "path": path, "roles": set()}
    )
    row["roles"].add(role)
    if layer and not row["layer"]:
        row["layer"] = layer
        row["language"] = language
    elif language and not row["language"]:
        row["language"] = language


def expected_plan(
    config, root: Path, paths: list[str], configured_root: str
) -> dict[str, object]:
    available = set(paths)
    rows: dict[str, dict[str, object]] = {}
    seen_layers: set[str] = set()
    for layer in config.layers:
        for header in layer.public_headers:
            add(
                rows,
                available,
                header,
                f"public-header:{layer.name}",
                language=layer.language,
            )
        for path in glob_files(root, layer.globs, config.exclude):
            relative = path.relative_to(root).as_posix()
            if relative in seen_layers:
                add(rows, available, relative, "source")
                continue
            seen_layers.add(relative)
            add(
                rows,
                available,
                relative,
                "source",
                layer=layer.name,
                language=layer.language,
            )
    for package in config.packages:
        add(rows, available, package.path, "manifest")
    for build in config.builds:
        add(rows, available, build.path, "build")
    for index in config.indexes:
        add(rows, available, index.path, "index")
    for artifact in config.artifacts:
        for pattern in artifact.inputs:
            for path in glob_files(root, (pattern,), config.exclude):
                add(
                    rows,
                    available,
                    path.relative_to(root).as_posix(),
                    "artifact-input",
                )
        for loader in artifact.loaded_by:
            for path in glob_files(root, loader.paths, config.exclude):
                add(
                    rows,
                    available,
                    path.relative_to(root).as_posix(),
                    "loader",
                )
    for bridge in config.bridges:
        for provider in bridge.providers:
            if provider.path:
                add(rows, available, provider.path, "provider")
    for test in config.tests:
        for path in glob_files(root, test.globs, config.exclude):
            add(
                rows,
                available,
                path.relative_to(root).as_posix(),
                "test",
                language=test.language,
            )
    for entry in config.named_entries:
        for path in glob_files(root, entry.globs, config.exclude):
            add(
                rows,
                available,
                path.relative_to(root).as_posix(),
                "named-entry",
            )
    for pattern in config.checks.forbid_paths:
        for path in glob_files(root, (pattern,), ()):
            add(
                rows,
                available,
                path.relative_to(root).as_posix(),
                "forbidden-check",
            )
    files = []
    for path in sorted(rows):
        row = rows[path]
        files.append(
            {
                "language": row["language"],
                "layer": row["layer"],
                "path": path,
                "roles": sorted(row["roles"]),
            }
        )
    return {
        "artifact": "archbird-discovery-plan",
        "configuration_sha256": config.digest,
        "files": files,
        "max_file_bytes": config.limits.max_file_bytes,
        "max_index_bytes": 536870912,
        "project": config.project,
        "root": configured_root,
        "schema_version": 1,
    }


def main() -> int:
    if len(sys.argv) < 4 or (len(sys.argv) - 2) % 2:
        raise SystemExit(
            "usage: test_discovery.py EXTENSION CONFIG ROOT [CONFIG ROOT ...]"
        )
    extension = load_extension(Path(sys.argv[1]).resolve())
    profiles = 0
    selected = 0
    for index in range(2, len(sys.argv), 2):
        config_path = Path(sys.argv[index]).resolve()
        root = Path(sys.argv[index + 1]).resolve()
        config = load_config(config_path, root_override=root)
        config_json = config_path.read_bytes()
        paths = inventory(extension, config_json, root)
        first = extension.discovery_plan(config_json, paths)
        second = extension.discovery_plan(config_json, list(reversed(paths)))
        if first != second:
            raise AssertionError(f"{config.project}: discovery depends on inventory order")
        actual = json.loads(first)
        configured_root = json.loads(config_json).get("root", ".")
        expected = expected_plan(config, root, paths, configured_root)
        if actual != expected:
            output = Path(__file__).resolve().parents[1] / "build"
            output.mkdir(parents=True, exist_ok=True)
            (output / f"{config.project}.native-discovery.json").write_text(
                json.dumps(actual, indent=2, sort_keys=True) + "\n"
            )
            (output / f"{config.project}.oracle-discovery.json").write_text(
                json.dumps(expected, indent=2, sort_keys=True) + "\n"
            )
            raise AssertionError(f"{config.project}: native discovery differs")
        profiles += 1
        selected += len(actual["files"])
    try:
        extension.discovery_plan(config_path.read_bytes(), ["../escape"])
    except Exception as error:
        if "canonical" not in str(error):
            raise
    else:
        raise AssertionError("noncanonical inventory path was accepted")

    default_exclude_config = json.dumps(
        {
            "schema_version": 1,
            "project": "default-excludes",
            "layers": [
                {
                    "name": "source",
                    "language": "python",
                    "globs": ["**/*"],
                }
            ],
            "artifacts": [
                {
                    "name": "package",
                    "output": "package.whl",
                    "inputs": ["**/*"],
                }
            ],
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    debris = [
        ".git/config",
        "build/generated.c",
        "nested/build/generated.c",
        "build-debug/generated.c",
        "nested/build-debug/generated.c",
        "dist/package.whl",
        "nested/dist/package.whl",
        "node_modules/pkg/index.js",
        "nested/node_modules/pkg/index.js",
        "venv/bin/python.py",
        ".venv/bin/python.py",
        "pkg/.venv/bin/python.py",
        "__pycache__/root.pyc",
        "pkg/__pycache__/module.pyc",
        ".pytest_cache/state",
        "pkg/.pytest_cache/state",
    ]
    default_plan = json.loads(
        extension.discovery_plan(
            default_exclude_config,
            ["pkg/main.py", "pkg/venv/__init__.py", *debris],
        )
    )
    selected_paths = [row["path"] for row in default_plan["files"]]
    if selected_paths != ["pkg/main.py", "pkg/venv/__init__.py"]:
        raise AssertionError(
            f"built-in excludes leaked generated repository debris: {selected_paths}"
        )
    descend = extension.discovery_descend(
        default_exclude_config,
        ["venv", "pkg/venv", ".venv", "pkg/.venv"],
    )
    if descend != [False, True, False, False]:
        raise AssertionError(f"built-in venv descent policy is incorrect: {descend}")
    print(
        f"native discovery parity passed: {profiles} profiles, "
        f"{selected} selected files; built-in excludes passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
