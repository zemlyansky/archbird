#!/usr/bin/env python3
"""End-to-end SCIP precision-provider ingestion through the native Map."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys

from archbird.providers.scip import scip_pb2
from archbird.providers.scip.reader import analyze_scip_index
from archbird.providers.python_ast import python_ast_provider_facts


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def write_index(path: Path) -> None:
    symbol = "scip pip demo 1.0 demo/add()."
    index = scip_pb2.Index()
    index.metadata.tool_info.name = "fixture-indexer"
    index.metadata.tool_info.version = "1.2.3"
    index.metadata.text_document_encoding = scip_pb2.UTF8
    core = index.documents.add()
    core.relative_path = "src/core.c"
    core.text = "int add(int a, int b) { return a+b; }\n"
    core.position_encoding = scip_pb2.UTF8CodeUnitOffsetFromLineStart
    definition = core.occurrences.add()
    definition.symbol = symbol
    definition.symbol_roles = scip_pb2.Definition
    definition.single_line_range.line = 0
    definition.single_line_range.start_character = 4
    definition.single_line_range.end_character = 7
    api = index.documents.add()
    api.relative_path = "py/api.py"
    api.text = "def invoke(a, b):\n    return add(a, b)\n"
    api.position_encoding = scip_pb2.UTF8CodeUnitOffsetFromLineStart
    reference = api.occurrences.add()
    reference.symbol = symbol
    reference.symbol_roles = scip_pb2.ReadAccess
    reference.single_line_range.line = 1
    reference.single_line_range.start_character = 11
    reference.single_line_range.end_character = 14
    for relative_path, encoding, start_character, end_character in (
        ("py/utf16.py", scip_pb2.UTF16CodeUnitOffsetFromLineStart, 6, 9),
        ("py/utf32.py", scip_pb2.UTF32CodeUnitOffsetFromLineStart, 5, 8),
    ):
        document = index.documents.add()
        document.relative_path = relative_path
        document.text = "🚀x = add\n"
        document.position_encoding = encoding
        occurrence = document.occurrences.add()
        occurrence.symbol = symbol
        occurrence.symbol_roles = scip_pb2.ReadAccess
        occurrence.single_line_range.line = 0
        occurrence.single_line_range.start_character = start_character
        occurrence.single_line_range.end_character = end_character
    path.write_bytes(index.SerializeToString())


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_scip_provider.py EXTENSION BUILD_ROOT")
    extension = load_extension(Path(sys.argv[1]).resolve())
    root = Path(sys.argv[2]).resolve() / "scip-provider-fixture"
    shutil.rmtree(root, ignore_errors=True)
    (root / "src").mkdir(parents=True)
    (root / "py").mkdir()
    (root / "src/core.c").write_text("int add(int a, int b) { return a+b; }\n")
    (root / "py/api.py").write_text("def invoke(a, b):\n    return add(a, b)\n")
    (root / "py/utf16.py").write_text("🚀x = add\n")
    (root / "py/utf32.py").write_text("🚀x = add\n")
    write_index(root / "index.scip")
    shutil.copyfile(root / "index.scip", root / "second.scip")
    config = {
        "schema_version": 1,
        "project": "scip-native",
        "layers": [
            {
                "name": "core",
                "role": "core",
                "language": "c",
                "globs": ["src/**/*.c"],
            },
            {
                "name": "python",
                "role": "frontend",
                "language": "python",
                "globs": ["py/**/*.py"],
            },
        ],
        "indexes": [
            {"name": "compiler", "format": "scip", "path": "index.scip"},
            {"name": "secondary", "format": "scip", "path": "second.scip"},
        ],
    }
    sources = {
        "index.scip": ("", "", ["index"], (root / "index.scip").read_bytes()),
        "second.scip": (
            "",
            "",
            ["index"],
            (root / "second.scip").read_bytes(),
        ),
        "py/api.py": (
            "python",
            "python",
            ["source"],
            (root / "py/api.py").read_bytes(),
        ),
        "py/utf16.py": (
            "python",
            "python",
            ["source"],
            (root / "py/utf16.py").read_bytes(),
        ),
        "py/utf32.py": (
            "python",
            "python",
            ["source"],
            (root / "py/utf32.py").read_bytes(),
        ),
        "src/core.c": (
            "core",
            "c",
            ["source"],
            (root / "src/core.c").read_bytes(),
        ),
    }
    manifest_files = []
    for path, (layer, language, roles, raw) in sorted(sources.items()):
        row = {
            "bytes": len(raw),
            "path": path,
            "roles": roles,
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
            "implementation_sha256": "a" * 64,
            "name": "scip-provider-test",
            "version": "1",
        },
        "project": "scip-native",
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    for path, (_layer, _language, _roles, raw) in sorted(sources.items()):
        extension.project_add_source(project, path, raw)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    for provider_id in ("lexical:c", "lexical:javascript", "lexical:r"):
        extension.project_scan_builtin_provider(
            project, provider_id, "primary"
        )
    manifest_sha = extension.project_manifest_sha256(project)
    extension.project_add_provider(
        project,
        "primary",
        python_ast_provider_facts(
            project="scip-native",
            path="py/api.py",
            text=sources["py/api.py"][3].decode(),
            source_manifest_sha256=manifest_sha,
        ),
    )
    evidence = analyze_scip_index(
        root / "index.scip",
        mapped_paths={"src/core.c", "py/api.py", "py/utf16.py", "py/utf32.py"},
    )
    extension.project_scan_builtin_provider(project, "semantic:scip", "primary")
    counts = extension.project_counts(project)
    semantic = json.loads(
        extension.project_provider_facts(project, counts["providers"] - 1)
    )
    definitions = [
        fact for fact in semantic["facts"] if fact["domain"] == "semantic-definitions"
    ]
    references = [
        fact for fact in semantic["facts"] if fact["domain"] == "reference-targets"
    ]
    assert not any(fact["domain"] == "index-edges" for fact in semantic["facts"])
    assert [fact["span"] for fact in definitions] == [{"end": 7, "start": 4}]
    assert sorted((fact["span"]["start"], fact["span"]["end"]) for fact in references) == [
        (8, 11),
        (8, 11),
        (29, 32),
    ]
    resolutions = {row["fact_id"]: row for row in semantic["resolutions"]}
    assert all(
        resolutions[fact["id"]]["state"] == "unique"
        and len(resolutions[fact["id"]]["targets"]) == 1
        for fact in references
    )
    extension.project_finalize_providers(project)
    assert extension.project_merge_summary(project)["conflicts"] == 0
    mapped = json.loads(extension.project_map(project))
    assert mapped["diagnostics"] == [], mapped["diagnostics"]
    assert [(row["name"], row["path"]) for row in mapped["indexes"]] == [
        ("compiler", "index.scip"),
        ("secondary", "second.scip"),
    ]
    for row in mapped["indexes"]:
        assert row["coverage"] == {
            "documents_mapped": evidence.coverage.documents_mapped,
            "documents_source_unverified": 0,
            "documents_source_verified": evidence.coverage.documents_mapped,
            "documents_stale": 0,
            "documents_total": evidence.coverage.documents_total,
            "edges": len(evidence.edges),
            "invalid_ranges": 0,
            "occurrences": evidence.coverage.occurrences,
            "position_encoding_fallback_documents": 0,
            "references": evidence.coverage.references,
            "reference_facts": 3,
            "relationship_edges": evidence.coverage.relationship_edges,
            "relationships": evidence.coverage.relationships,
            "resolved_ambiguous": evidence.coverage.resolved_ambiguous,
            "resolved_unique": evidence.coverage.resolved_unique,
            "source_mismatches": 0,
            "symbols": evidence.coverage.symbols,
            "unresolved": evidence.coverage.unresolved,
        }
        assert row["position_encoding_fallback"] is None
        assert row["sha256"] == evidence.metadata.source_sha256
    semantic_edges = [
        row for row in mapped["edges"] if row["kind"] == "semantic-reference"
    ]
    assert semantic_edges == [
        {
            "evidence": [
                {
                    "basis": "semantic-index",
                    "provider": "compiler",
                    "state": "current",
                },
                {
                    "basis": "semantic-index",
                    "provider": "secondary",
                    "state": "current",
                },
            ],
            "kind": "semantic-reference",
            "names": ["add"],
            "source": "py/api.py",
            "target": "src/core.c",
        },
        {
            "evidence": [
                {
                    "basis": "semantic-index",
                    "provider": "compiler",
                    "state": "current",
                },
                {
                    "basis": "semantic-index",
                    "provider": "secondary",
                    "state": "current",
                },
            ],
            "kind": "semantic-reference",
            "names": ["add"],
            "source": "py/utf16.py",
            "target": "src/core.c",
        },
        {
            "evidence": [
                {
                    "basis": "semantic-index",
                    "provider": "compiler",
                    "state": "current",
                },
                {
                    "basis": "semantic-index",
                    "provider": "secondary",
                    "state": "current",
                },
            ],
            "kind": "semantic-reference",
            "names": ["add"],
            "source": "py/utf32.py",
            "target": "src/core.c",
        },
    ]
    print("native SCIP provider Map parity passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
