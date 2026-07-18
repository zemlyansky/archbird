#!/usr/bin/env python3
"""Differential package-surface aggregation through the native map."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.analyze import analyze, glob_files
from archbird.map.config import load_config


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


def package_row(package) -> dict:
    return {
        "aliases": sorted(package.aliases),
        "dependencies": [
            {"name": name, "requirement": requirement, "scope": scope}
            for name, (requirement, scope) in sorted(package.dependencies.items())
        ],
        "entrypoints": dict(sorted(package.entrypoints.items())),
        "export_origins": {
            name: list(paths)
            for name, paths in sorted(package.export_origins.items())
        },
        "exports": sorted(package.exports),
        "identity": package.identity,
        "kind": package.kind,
        "layer": package.layer,
        "manifest": package.manifest,
        "name": package.name,
        "scripts": dict(sorted(package.scripts.items())),
        "version": package.version,
    }


def build_row(route) -> dict:
    return {
        "command": route.command,
        "conditions": list(route.conditions),
        "deps": list(route.deps),
        "name": route.name,
        "paths": list(route.paths),
        "source": route.source,
    }


def artifact_row(artifact) -> dict:
    return {
        "builds": [
            {"source": row.source, "target": row.target}
            for row in artifact.builds
        ],
        "depends_on": list(artifact.depends_on),
        "inputs": [
            {"evidence": list(row.evidence), "path": row.path}
            for row in artifact.inputs
        ],
        "loaded_by": [
            {"matches": row.matches, "path": row.path, "pattern": row.pattern}
            for row in artifact.loaded_by
        ],
        "name": artifact.name,
        "output": artifact.output,
    }


def surface_row(surface) -> dict:
    active = [row for row in surface.names if not row.ignored]
    return {
        "name": surface.name,
        "kind": surface.kind,
        "provider_configured": surface.provider_configured,
        "providers": [
            {"path": row.path, "source": row.source} for row in surface.providers
        ],
        "summary": {
            "registered": sum(row.declaration == "declared" for row in active),
            "used": sum(bool(row.uses) for row in active),
            "unused": sum(
                row.declaration == "declared" and not row.uses for row in active
            ),
            "unregistered_use": sum(
                row.declaration == "undeclared" and bool(row.uses)
                for row in active
            ),
            "declaration_unknown": sum(
                row.declaration == "unknown" for row in active
            ),
            "resolved": sum(row.resolution == "unique" for row in active),
            "unresolved": sum(row.resolution == "unresolved" for row in active),
            "ambiguous": sum(row.resolution == "ambiguous" for row in active),
            "ignored": sum(row.ignored for row in surface.names),
        },
        "names": [
            {
                "name": row.name,
                "declaration": row.declaration,
                "registered": row.declaration == "declared",
                "used": bool(row.uses),
                "unused": row.declaration == "declared" and not row.uses,
                "unregistered_use": row.declaration == "undeclared"
                and bool(row.uses),
                "declarations": [
                    {"path": item.path, "source": item.source}
                    for item in row.declarations
                ],
                "uses": [
                    {"path": item.path, "count": item.count} for item in row.uses
                ],
                "candidates": list(row.candidates),
                "resolution": row.resolution,
                "declaration_signatures": list(row.declaration_signatures),
                "implementation_signatures": list(row.implementation_signatures),
                "ignored": row.ignored,
            }
            for row in surface.names
        ],
    }


def test_row(test) -> dict:
    return {
        "path": test.path,
        "group": test.group,
        "language": test.language,
        "framework": test.framework,
        "count": test.count,
        "selectors": list(test.selectors),
        "routes": dict(sorted(test.routes.items())),
        "cases": [
            {
                "selector": case.selector,
                "line": case.line,
                "routes": dict(sorted(case.routes.items())),
                "configured_routes": list(case.configured_routes),
            }
            for case in test.cases
        ],
    }


def native_test_projection(test: dict) -> dict:
    """Project richer native evidence onto the immutable oracle's shape."""

    return {
        "path": test["path"],
        "group": test["group"],
        "language": test["language"],
        "framework": test["framework"],
        "count": test["count"],
        "selectors": test["selectors"],
        "routes": test["routes"],
        "cases": [
            {
                "selector": case["selector"],
                "line": case["line"],
                "routes": case["routes"],
                "configured_routes": case["configured_routes"],
            }
            for case in test["cases"]
        ],
    }


def parity_row(parity) -> dict:
    return {
        "name": parity.name,
        "enforce": parity.enforce,
        "shared": sorted(parity.shared),
        "members": [
            {
                "label": member.label,
                "values": sorted(member.values),
                "evidence": {
                    name: list(paths)
                    for name, paths in sorted(member.evidence.items())
                },
                "missing": sorted(parity.missing[member.label]),
            }
            for member in parity.members
        ],
    }


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_map_packages.py EXTENSION ROOT FIXTURE")
    root = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    extension = load_module("archbird._native", Path(sys.argv[1]).resolve())
    provider = load_module(
        "archbird_native_python_ast", root / "py/archbird/providers/python_ast.py"
    )
    config_path = fixture / "archbird.json"
    config = load_config(config_path, root_override=fixture)

    sources: dict[str, tuple[str | None, str | None, set[str], bytes]] = {}
    for layer in config.layers:
        for path in glob_files(fixture, layer.globs, config.exclude):
            relative = path.relative_to(fixture).as_posix()
            sources[relative] = (
                layer.name,
                layer.language,
                {"source"},
                path.read_bytes(),
            )
    for package in config.packages:
        path = fixture / package.path
        previous = sources.get(package.path)
        if previous:
            layer, language, roles, raw = previous
            roles.add("manifest")
            sources[package.path] = (layer, language, roles, raw)
        else:
            sources[package.path] = (
                None,
                None,
                {"manifest"},
                path.read_bytes(),
            )
    for build in config.builds:
        path = fixture / build.path
        previous = sources.get(build.path)
        if previous:
            layer, language, roles, raw = previous
            roles.add("build")
            sources[build.path] = (layer, language, roles, raw)
        else:
            sources[build.path] = (
                None,
                None,
                {"build"},
                path.read_bytes(),
            )
    for artifact in config.artifacts:
        for pattern in artifact.inputs:
            for path in glob_files(fixture, (pattern,), config.exclude):
                relative = path.relative_to(fixture).as_posix()
                previous = sources.get(relative)
                if previous:
                    layer, language, roles, raw = previous
                    roles.add("artifact-input")
                    sources[relative] = (layer, language, roles, raw)
                else:
                    sources[relative] = (
                        None,
                        None,
                        {"artifact-input"},
                        path.read_bytes(),
                    )
        for loader in artifact.loaded_by:
            for path in glob_files(fixture, loader.paths, config.exclude):
                relative = path.relative_to(fixture).as_posix()
                previous = sources.get(relative)
                if previous:
                    layer, language, roles, raw = previous
                    roles.add("loader")
                    sources[relative] = (layer, language, roles, raw)
                else:
                    sources[relative] = (
                        None,
                        None,
                        {"loader"},
                        path.read_bytes(),
                    )
    for test in config.tests:
        for path in glob_files(fixture, test.globs, config.exclude):
            relative = path.relative_to(fixture).as_posix()
            previous = sources.get(relative)
            if previous:
                layer, language, roles, raw = previous
                roles.add("test")
                sources[relative] = (layer, language or test.language, roles, raw)
            else:
                sources[relative] = (
                    None,
                    test.language,
                    {"test"},
                    path.read_bytes(),
                )

    manifest_files = []
    for path, (layer, language, roles, raw) in sorted(sources.items()):
        row = {
            "bytes": len(raw),
            "path": path,
            "roles": sorted(roles),
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
        if layer:
            row["layer"] = layer
        if language:
            row["language"] = language
        manifest_files.append(row)
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": manifest_files,
        "producer": {
            "implementation_sha256": "8" * 64,
            "name": "map-package-test-host",
            "version": "1",
        },
        "project": config.project,
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    for path, (_layer, _language, _roles, raw) in sorted(sources.items()):
        extension.project_add_source(project, path, raw)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, config_path.read_bytes())
    for provider_id in ("lexical:c", "lexical:javascript", "lexical:r"):
        extension.project_scan_builtin_provider(
            project, provider_id, "primary"
        )
    for path, (layer, language, _roles, raw) in sorted(sources.items()):
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
    expected = [package_row(package) for package in oracle.packages.values()]
    # The immutable Python oracle records the entry file for a re-export and
    # cannot distinguish it from a local export. Native facts retain the
    # explicit ESM source, so the resolved origin is the target module.
    npm_expected = next(row for row in expected if row["name"] == "npm")
    assert npm_expected["export_origins"]["feature"] == ["js/src/index.js"]
    npm_expected["export_origins"]["feature"] = ["js/src/feature.js"]
    assert native["packages"] == expected, (native["packages"], expected)
    assert native["builds"] == [build_row(route) for route in oracle.builds]
    expected_artifacts = [artifact_row(artifact) for artifact in oracle.artifacts]
    assert native["artifacts"] == expected_artifacts, (
        native["artifacts"],
        expected_artifacts,
    )
    assert [row["path"] for row in native["files"]] == sorted(oracle.files)
    expected_surfaces = [
        surface_row(surface) for surface in oracle.surfaces.values()
    ]
    assert native["surfaces"] == expected_surfaces, (
        native["surfaces"],
        expected_surfaces,
    )
    expected_edges = [
        {
            "kind": row.kind,
            "names": list(row.names),
            "source": row.source,
            "target": row.target,
        }
        for row in oracle.edges
    ]
    # The host AST emits imported-name call evidence that the immutable
    # lexical oracle does not model. Keep the independently source-verified
    # edge as an intentional precision gain.
    assert not any(row["kind"] == "imported-call" for row in expected_edges)
    expected_edges.append(
        {
            "kind": "imported-call",
            "names": ["main"],
            "source": "test/test_python.py",
            "target": "py/pkg/api.py",
        }
    )
    assert native["edges"] == expected_edges, (native["edges"], expected_edges)
    expected_tests = [test_row(test) for test in oracle.tests]
    assert [native_test_projection(row) for row in native["tests"]] == expected_tests, (
        native["tests"],
        expected_tests,
    )
    assert all(row["count_unit"] == "static_case_occurrence" for row in native["tests"])
    assert all("route_evidence" in row for row in native["tests"])
    assert native["named_entries"] == {
        name: {path: list(values) for path, values in by_path.items()}
        for name, by_path in oracle.named_entries.items()
    }
    expected_parity = [parity_row(parity) for parity in oracle.parity]
    assert native["parity"] == expected_parity, (
        native["parity"],
        expected_parity,
    )
    assert native["evidence"]["input_sha256"] == oracle.input_digest
    expected_diagnostics = [
        {
            "code": row.code,
            "message": row.message,
            "path": row.path,
            "severity": row.severity,
        }
        for row in oracle.diagnostics
    ]
    assert native["diagnostics"] == expected_diagnostics, (
        native["diagnostics"],
        expected_diagnostics,
    )
    print(f"native package Map parity passed: {len(expected)} package surfaces")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
