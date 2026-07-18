#!/usr/bin/env python3
"""Exact aggregate Map differentials on maintained real repository profiles."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.analyze import analyze, glob_files
from archbird.map.config import load_config
from archbird.map.render import _map_dict


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def add_source(
    sources: dict[str, dict[str, object]],
    root: Path,
    path: Path,
    role: str,
    *,
    layer: str = "",
    language: str = "",
) -> None:
    if not path.is_file():
        return
    relative = path.relative_to(root).as_posix()
    row = sources.setdefault(
        relative,
        {"layer": "", "language": "", "roles": set(), "raw": path.read_bytes()},
    )
    row["roles"].add(role)
    if layer and not row["layer"]:
        row["layer"] = layer
        row["language"] = language
    elif language and not row["language"]:
        row["language"] = language


def discover(config, root: Path) -> dict[str, dict[str, object]]:
    sources: dict[str, dict[str, object]] = {}
    seen_layers: set[str] = set()
    for layer in config.layers:
        for header in layer.public_headers:
            add_source(sources, root, root / header, "public-header")
        for path in glob_files(root, layer.globs, config.exclude):
            relative = path.relative_to(root).as_posix()
            if relative in seen_layers:
                add_source(sources, root, path, "source")
                continue
            seen_layers.add(relative)
            add_source(
                sources,
                root,
                path,
                "source",
                layer=layer.name,
                language=layer.language,
            )
    for package in config.packages:
        add_source(sources, root, root / package.path, "manifest")
    for build in config.builds:
        add_source(sources, root, root / build.path, "build")
    for index in config.indexes:
        add_source(sources, root, root / index.path, "index")
    for artifact in config.artifacts:
        for pattern in artifact.inputs:
            for path in glob_files(root, (pattern,), config.exclude):
                add_source(sources, root, path, "artifact-input")
        for loader in artifact.loaded_by:
            for path in glob_files(root, loader.paths, config.exclude):
                add_source(sources, root, path, "loader")
    for bridge in config.bridges:
        for provider in bridge.providers:
            if provider.path:
                add_source(sources, root, root / provider.path, "provider")
    for test in config.tests:
        for path in glob_files(root, test.globs, config.exclude):
            add_source(
                sources,
                root,
                path,
                "test",
                language=test.language,
            )
    for entry in config.named_entries:
        for path in glob_files(root, entry.globs, config.exclude):
            add_source(sources, root, path, "named-entry")
    for pattern in config.checks.forbid_paths:
        for path in glob_files(root, (pattern,), ()):
            add_source(sources, root, path, "forbidden-check")
    return sources


def native_map(extension, provider, config, root: Path) -> dict:
    sources = discover(config, root)
    manifest_files = []
    for path, source in sorted(sources.items()):
        raw = source["raw"]
        row = {
            "bytes": len(raw),
            "path": path,
            "roles": sorted(source["roles"]),
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
        if source["layer"]:
            row["layer"] = source["layer"]
        if source["language"]:
            row["language"] = source["language"]
        manifest_files.append(row)
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": manifest_files,
        "producer": {
            "implementation_sha256": "9" * 64,
            "name": "map-profile-host",
            "version": "1",
        },
        "project": config.project,
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    for path, source in sorted(sources.items()):
        extension.project_add_source(project, path, source["raw"])
    extension.project_finalize_sources(project)
    extension.project_set_config(project, config.path.read_bytes())
    for provider_id in ("lexical:c", "lexical:javascript", "lexical:r"):
        extension.project_scan_builtin_provider(
            project, provider_id, "primary"
        )
    manifest_sha = extension.project_manifest_sha256(project)
    for path, source in sorted(sources.items()):
        if source["language"] != "python":
            continue
        extension.project_add_provider(
            project,
            "primary",
            provider.python_ast_provider_facts(
                project=config.project,
                path=path,
                text=source["raw"].decode("utf-8"),
                source_manifest_sha256=manifest_sha,
            ),
        )
    extension.project_finalize_providers(project)
    return json.loads(extension.project_map(project))


def comparable(document: dict) -> dict:
    result = dict(document)
    result.pop("tool", None)
    return result


def main() -> int:
    if len(sys.argv) < 6 or (len(sys.argv) - 4) % 2:
        raise SystemExit(
            "usage: test_map_profiles.py EXTENSION NATIVE_ROOT ORACLE_ROOT "
            "CONFIG ROOT [CONFIG ROOT ...]"
        )
    extension = load_module("archbird._native", Path(sys.argv[1]).resolve())
    native_root = Path(sys.argv[2]).resolve()
    provider = load_module(
        "archbird_native_python_ast",
        native_root / "py/archbird/providers/python_ast.py",
    )
    profiles = 0
    files = 0
    for index in range(4, len(sys.argv), 2):
        config_path = Path(sys.argv[index]).resolve()
        root = Path(sys.argv[index + 1]).resolve()
        config = load_config(config_path, root_override=root)
        native = native_map(extension, provider, config, root)
        expected = _map_dict(analyze(config))
        if comparable(native) != comparable(expected):
            left = native_root / "build" / f"{config.project}.native-map.json"
            right = native_root / "build" / f"{config.project}.oracle-map.json"
            left.parent.mkdir(parents=True, exist_ok=True)
            left.write_text(json.dumps(native, indent=2, sort_keys=True) + "\n")
            right.write_text(json.dumps(expected, indent=2, sort_keys=True) + "\n")
            raise AssertionError(
                f"{config.project}: aggregate Map differs; inspect {left} and {right}"
            )
        profiles += 1
        files += len(native["files"])
    print(f"native aggregate Map parity passed: {profiles} profiles, {files} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
