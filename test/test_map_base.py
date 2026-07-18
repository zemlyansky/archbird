#!/usr/bin/env python3
"""Differential base Map aggregation over native provider facts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.analyze import analyze, glob_files
from archbird.map.config import load_config


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


def file_row(facts) -> dict:
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


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_map_base.py EXTENSION NATIVE_ROOT FIXTURE")
    extension = load_extension(Path(sys.argv[1]).resolve())
    native_root = Path(sys.argv[2]).resolve()
    provider = load_provider(native_root)
    fixture = Path(sys.argv[3]).resolve()
    config_path = fixture / "archbird.json"
    config = load_config(config_path, root_override=fixture)
    sources = []
    for layer in config.layers:
        for path in glob_files(fixture, layer.globs, config.exclude):
            relative = path.relative_to(fixture).as_posix()
            raw = path.read_bytes()
            sources.append((relative, layer.name, layer.language, raw))
    sources.sort(key=lambda row: row[0])
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(raw),
                "language": language,
                "layer": layer,
                "path": path,
                "roles": ["source"],
                "sha256": hashlib.sha256(raw).hexdigest(),
            }
            for path, layer, language, raw in sources
        ],
        "producer": {
            "implementation_sha256": "3" * 64,
            "name": "map-test-host",
            "version": "1",
        },
        "project": config.project,
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    for path, _layer, _language, raw in sources:
        extension.project_add_source(project, path, raw)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, config_path.read_bytes())
    for provider_id in ("lexical:c", "lexical:javascript", "lexical:r"):
        extension.project_scan_builtin_provider(
            project, provider_id, "primary"
        )
    for path, _layer, language, raw in sources:
        if language != "python":
            continue
        extension.project_add_provider(
            project,
            "primary",
            provider.python_ast_provider_facts(
                project=config.project,
                path=path,
                text=raw.decode(),
                source_manifest_sha256=extension.project_manifest_sha256(project),
            ),
        )
    extension.project_finalize_providers(project)
    native = json.loads(extension.project_map(project))
    oracle = analyze(config)
    wanted_files = [file_row(oracle.files[path]) for path in sorted(oracle.files)]
    wanted_layers = [
        {
            "files": sum(1 for row in oracle.files.values() if row.layer == name),
            "language": oracle.layer_languages[name],
            "name": name,
            "role": oracle.layer_roles[name],
            "symbols": sum(
                len(row.symbols) for row in oracle.files.values() if row.layer == name
            ),
        }
        for name in sorted(oracle.layer_roles)
    ]
    wanted_components = [
        {
            "description": component.description,
            "files": list(component.files),
            "name": component.name,
            "outgoing": {
                name: sorted(values)
                for name, values in sorted(component.outgoing.items())
            },
            "symbol_count": component.symbol_count,
        }
        for component in sorted(oracle.components.values(), key=lambda row: row.name)
    ]
    wanted_edges = [
        {
            "kind": edge.kind,
            "names": list(edge.names),
            "source": edge.source,
            "target": edge.target,
        }
        for edge in oracle.edges
    ]
    wanted_resolutions = [
        {
            "candidates": list(row.candidates),
            "count": row.count,
            "kind": row.kind,
            "name": row.name,
            "source": row.source,
        }
        for row in oracle.call_resolutions
    ]
    assert native["files"] == wanted_files
    assert native["layers"] == wanted_layers
    assert native["components"] == wanted_components
    assert native["edges"] == wanted_edges
    assert native["call_resolutions"] == wanted_resolutions
    assert native["description"] == oracle.description
    assert native["evidence"] == {
        "absolute_paths_included": False,
        "config_sha256": oracle.config_digest,
        "input_sha256": oracle.input_digest,
    }
    assert native["limits"] == {
        "compact_edge_names": oracle.compact_edge_names,
        "compact_symbols": oracle.compact_symbols,
    }
    assert native["project"] == oracle.project
    assert native["schema_version"] == 7
    first = extension.project_map(project)
    second = extension.project_map(project)
    assert first == second
    print(
        f"native base Map parity passed: {len(wanted_files)} files, "
        f"{sum(row['bytes'] for row in wanted_files)} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
