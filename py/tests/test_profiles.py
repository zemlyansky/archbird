#!/usr/bin/env python3
"""Compare native CPython provider reductions across configured profiles."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.analyze import glob_files
from archbird.map.config import load_config
from archbird.map.scanners import python_file_facts


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def load_provider(root: Path):
    path = root / "py/archbird/providers/python_ast.py"
    spec = importlib.util.spec_from_file_location("archbird_native_python_ast", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load Python provider {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def expected(facts) -> dict:
    return {
        "bytes": facts.bytes,
        "call_counts": facts.call_counts,
        "calls": sorted(facts.calls),
        "export_origins": facts.export_origins,
        "exports": sorted(facts.exports),
        "imported_names": {
            key: sorted(values) for key, values in facts.imported_names.items()
        },
        "imports": sorted(facts.imports),
        "language": facts.language,
        "layer": facts.layer,
        "messages": {
            "receives": sorted(facts.receives),
            "sends": sorted(facts.sends),
        },
        "method_call_counts": facts.method_call_counts,
        "method_calls": sorted(facts.method_calls),
        "path": facts.path,
        "reexport_candidates": sorted(facts.reexport_candidates),
        "sha256": facts.sha256,
        "symbols": [
            {
                "kind": row.kind,
                "line": row.line,
                "name": row.name,
                "scope": row.scope,
                "signature": row.signature,
            }
            for row in facts.symbols
        ],
    }


def scan_file(extension, provider, project_name: str, relative: str, layer: str, raw: bytes):
    digest = hashlib.sha256(raw).hexdigest()
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(raw),
                "language": "python",
                "layer": layer,
                "path": relative,
                "roles": ["source"],
                "sha256": digest,
            }
        ],
        "producer": {
            "implementation_sha256": "2" * 64,
            "name": "python-profile-host",
            "version": "1",
        },
        "project": project_name,
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    extension.project_add_source(project, relative, raw)
    extension.project_finalize_sources(project)
    provider_json = provider.python_ast_provider_facts(
        project=project_name,
        path=relative,
        text=raw.decode("utf-8"),
        source_manifest_sha256=extension.project_manifest_sha256(project),
    )
    extension.project_add_provider(project, "primary", provider_json)
    extension.project_finalize_providers(project)
    return json.loads(extension.project_file_facts(project))["files"][0]


def scan_profile(extension, provider, config_path: Path, root: Path) -> tuple[int, int]:
    config = load_config(config_path, root_override=root)
    checked = 0
    byte_count = 0
    for layer in config.layers:
        if layer.language != "python":
            continue
        for path in glob_files(root, layer.globs, config.exclude):
            relative = path.relative_to(root).as_posix()
            raw = path.read_bytes()
            native = scan_file(
                extension, provider, config.project, relative, layer.name, raw
            )
            oracle = python_file_facts(
                relative,
                layer.name,
                raw.decode("utf-8"),
                hashlib.sha256(raw).hexdigest(),
            )
            wanted = expected(oracle)
            if native != wanted:
                raise AssertionError(
                    f"{config.project}:{relative}: native Python facts differ\n"
                    f"native={native!r}\noracle={wanted!r}"
                )
            checked += 1
            byte_count += len(raw)
    return checked, byte_count


def main() -> int:
    if len(sys.argv) < 5 or (len(sys.argv) - 3) % 2:
        raise SystemExit(
            "usage: test_python_profiles.py EXTENSION NATIVE_ROOT CONFIG ROOT "
            "[CONFIG ROOT ...]"
        )
    extension = load_extension(Path(sys.argv[1]).resolve())
    provider = load_provider(Path(sys.argv[2]).resolve())
    total_files = 0
    total_bytes = 0
    profiles = 0
    for index in range(3, len(sys.argv), 2):
        files, byte_count = scan_profile(
            extension,
            provider,
            Path(sys.argv[index]).resolve(),
            Path(sys.argv[index + 1]).resolve(),
        )
        total_files += files
        total_bytes += byte_count
        profiles += 1
    print(
        f"native Python profile parity passed: {profiles} profiles, "
        f"{total_files} files, {total_bytes} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
