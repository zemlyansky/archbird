#!/usr/bin/env python3
"""Host syntax rejection must not poison successful portable evidence."""

from __future__ import annotations

import ast
import hashlib
import json
import warnings

from archbird import _native
from archbird.providers import python_ast as provider


SOURCE = """\
class Model:
    def run(self, value):
        return value

def train(model, item):
    return model.run(item)
"""


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def main() -> int:
    inherited_enum = json.loads(
        provider.python_ast_provider_facts(
            project="provider-applicability",
            path="pkg/inherited_enum.py",
            source_bytes=(
                "from enum import IntEnum as BaseIntEnum, auto\n"
                "class FastEnum(BaseIntEnum):\n"
                "    pass\n"
                "class Ops(FastEnum):\n"
                "    ADD = auto()\n"
                "    MUL = auto()\n"
            ).encode(),
        )
    )
    inherited_members = {
        row["name"]: row["attributes"]["value"]
        for row in inherited_enum["facts"]
        if row["domain"] == "constant-values"
        and row["kind"] == "enum-member"
        and row["attributes"]["container"] == "Ops"
    }
    if inherited_members != {"ADD": 1, "MUL": 2}:
        raise AssertionError(inherited_members)

    import_source = "import a, b.c as d\nfrom .x import (y,\n z as q,)\n"
    import_raw = import_source.encode("utf-8")
    import_tree = ast.parse(import_source)
    expected_alias_spans = [((7, 8), (10, 18)), ((35, 36), (39, 45))]
    for statement in import_tree.body:
        for alias in statement.names:
            alias.lineno = None
            alias.col_offset = None
            alias.end_lineno = None
            alias.end_col_offset = None
    positions = provider._SourcePositions(
        import_source, import_raw, provider._line_starts(import_raw)
    )
    actual_alias_spans = [
        positions.import_aliases(statement) for statement in import_tree.body
    ]
    if actual_alias_spans != expected_alias_spans:
        raise AssertionError((actual_alias_spans, expected_alias_spans))

    encoded_raw = (
        b"# coding: iso-8859-1\n"
        b"label = 'caf\xe9'\n"
        b"def r\xe9sum\xe9():\n"
        b"    return label\n"
    )
    encoded_document = json.loads(
        provider.python_ast_provider_facts(
            project="provider-applicability",
            path="pkg/encoded.py",
            source_bytes=encoded_raw,
        )
    )
    encoded_symbol = next(
        row
        for row in encoded_document["facts"]
        if row["domain"] == "symbols" and row.get("name") == "r\xe9sum\xe9"
    )
    encoded_start = encoded_raw.index(b"r\xe9sum\xe9")
    if encoded_symbol["span"] != {
        "end": encoded_start + len(b"r\xe9sum\xe9"),
        "start": encoded_start,
    }:
        raise AssertionError(encoded_symbol)
    if encoded_document["inputs"][0]["source_sha256"] != hashlib.sha256(
        encoded_raw
    ).hexdigest():
        raise AssertionError("encoded provider did not bind exact source bytes")

    invalid_encoding_raw = b'value = "b\xf6se"\n'
    invalid_encoding_document = json.loads(
        provider.python_ast_provider_facts(
            project="provider-applicability",
            path="pkg/invalid_encoding.py",
            source_bytes=invalid_encoding_raw,
        )
    )
    if invalid_encoding_document["facts"] or {
        row["coverage"] for row in invalid_encoding_document["capabilities"]
    } != {"none"}:
        raise AssertionError("undecodable Python source claimed AST coverage")
    if [row["code"] for row in invalid_encoding_document["diagnostics"]] != [
        "python-ast-encoding-inapplicable"
    ]:
        raise AssertionError(invalid_encoding_document["diagnostics"])

    undecodable_token = b"\xf6 = 1\n"
    lexical_path = "pkg/undecodable_token.py"
    lexical_manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(undecodable_token),
                "language": "python",
                "layer": "python",
                "path": lexical_path,
                "roles": ["source"],
                "sha256": hashlib.sha256(undecodable_token).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "provider-applicability-fixture",
            "version": "1",
        },
        "project": "provider-applicability",
        "schema_version": 1,
    }
    lexical_project = _native.project_create(canonical(lexical_manifest))
    _native.project_add_source(
        lexical_project, lexical_path, undecodable_token
    )
    _native.project_finalize_sources(lexical_project)
    _native.project_scan_builtin_provider(
        lexical_project, "lexical:python", "augment"
    )
    lexical_document = json.loads(
        _native.project_provider_facts(lexical_project, 0)
    )
    if lexical_document["facts"] or {
        row["coverage"] for row in lexical_document["capabilities"]
    } != {"none"}:
        raise AssertionError("undecodable Python token claimed lexical coverage")
    if [row["code"] for row in lexical_document["diagnostics"]] != [
        "python-lexical-encoding-inapplicable"
    ]:
        raise AssertionError(lexical_document["diagnostics"])

    encoded_path = "pkg/encoded.py"
    encoded_manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(encoded_raw),
                "language": "python",
                "layer": "python",
                "path": encoded_path,
                "roles": ["source"],
                "sha256": hashlib.sha256(encoded_raw).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "provider-applicability-fixture",
            "version": "1",
        },
        "project": "provider-applicability",
        "schema_version": 1,
    }
    encoded_project = _native.project_create(canonical(encoded_manifest))
    _native.project_add_source(encoded_project, encoded_path, encoded_raw)
    _native.project_finalize_sources(encoded_project)
    _native.project_scan_builtin_provider(
        encoded_project, "syntax:tree-sitter:python", "augment"
    )
    encoded_tree_document = json.loads(
        _native.project_provider_facts(encoded_project, 0)
    )
    if encoded_tree_document["facts"] or {
        row["coverage"] for row in encoded_tree_document["capabilities"]
    } != {"none"}:
        raise AssertionError("non-UTF-8 Python claimed Tree-sitter coverage")
    if [row["code"] for row in encoded_tree_document["diagnostics"]] != [
        "tree-sitter-python-encoding-inapplicable"
    ]:
        raise AssertionError(encoded_tree_document["diagnostics"])

    normalized_source = "def f():\n    ｗｗｗ = 1\n    return ｗｗｗ()\n"
    normalized_raw = normalized_source.encode("utf-8")
    normalized_path = "pkg/normalized.py"
    normalized_host = provider.python_ast_provider_facts(
        project="provider-applicability",
        path=normalized_path,
        source_bytes=normalized_raw,
    )
    normalized_host_document = json.loads(normalized_host)
    normalized_call = next(
        row
        for row in normalized_host_document["facts"]
        if row["domain"] == "calls" and row.get("name") == "www"
    )
    if normalized_call.get("attributes", {}).get("source_name") != "ｗｗｗ":
        raise AssertionError(normalized_call)
    normalized_manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(normalized_raw),
                "language": "python",
                "layer": "python",
                "path": normalized_path,
                "roles": ["source"],
                "sha256": hashlib.sha256(normalized_raw).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "provider-applicability-fixture",
            "version": "1",
        },
        "project": "provider-applicability",
        "schema_version": 1,
    }
    normalized_project = _native.project_create(canonical(normalized_manifest))
    _native.project_add_source(
        normalized_project, normalized_path, normalized_raw
    )
    _native.project_finalize_sources(normalized_project)
    _native.project_add_provider(normalized_project, "primary", normalized_host)
    _native.project_scan_builtin_provider(
        normalized_project, "lexical:python", "augment"
    )
    _native.project_scan_builtin_provider(
        normalized_project, "syntax:tree-sitter:python", "augment"
    )
    _native.project_finalize_providers(normalized_project)
    normalized_facts = json.loads(
        _native.project_file_facts(normalized_project)
    )["files"][0]
    if normalized_facts["calls"] != ["www"]:
        raise AssertionError(normalized_facts["calls"])

    raw = SOURCE.encode()
    path = "pkg/portable.py"
    original_warning_parse = provider.ast.parse
    original_warning_symtable = provider.symtable.symtable
    try:

        def parse_with_warning(*args, **kwargs):
            warnings.warn("fixture AST warning", SyntaxWarning)
            return original_warning_parse(*args, **kwargs)

        def symtable_with_warning(*args, **kwargs):
            warnings.warn("fixture symtable warning", SyntaxWarning)
            return original_warning_symtable(*args, **kwargs)

        provider.ast.parse = parse_with_warning
        provider.symtable.symtable = symtable_with_warning
        with warnings.catch_warnings(record=True) as emitted:
            warnings.simplefilter("always")
            provider.python_ast_provider_facts(
                project="provider-applicability",
                path=path,
                text=SOURCE,
            )
    finally:
        provider.ast.parse = original_warning_parse
        provider.symtable.symtable = original_warning_symtable
    leaked = [row for row in emitted if issubclass(row.category, SyntaxWarning)]
    if leaked:
        raise AssertionError(f"CPython parser warnings leaked: {leaked!r}")
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(raw),
                "language": "python",
                "layer": "python",
                "path": path,
                "roles": ["source"],
                "sha256": hashlib.sha256(raw).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "provider-applicability-fixture",
            "version": "1",
        },
        "project": "provider-applicability",
        "schema_version": 1,
    }
    project = _native.project_create(canonical(manifest))
    _native.project_add_source(project, path, raw)
    _native.project_finalize_sources(project)
    _native.project_set_config(
        project,
        canonical(
            {
                "description": "optional host syntax applicability fixture",
                "layers": [
                    {
                        "globs": ["pkg/**/*.py"],
                        "language": "python",
                        "name": "python",
                        "role": "core",
                    }
                ],
                "project": "provider-applicability",
                "schema_version": 2,
            }
        ),
    )

    original_parse = provider.ast.parse
    try:

        def reject_source(*args, **kwargs):
            del args, kwargs
            raise SyntaxError(
                "fixture host grammar is older",
                (path, 1, 1, "class Model:\n"),
            )

        provider.ast.parse = reject_source
        host_facts = provider.python_ast_provider_facts(
            project="provider-applicability",
            path=path,
            text=SOURCE,
        )
    finally:
        provider.ast.parse = original_parse

    host_document = json.loads(host_facts)
    if host_document["facts"]:
        raise AssertionError("rejected host syntax unexpectedly produced facts")
    if {row["coverage"] for row in host_document["capabilities"]} != {"none"}:
        raise AssertionError("rejected host syntax claimed coverage")
    if [
        (row["code"], row["severity"])
        for row in host_document["diagnostics"]
    ] != [("python-ast-inapplicable", "note")]:
        raise AssertionError(host_document["diagnostics"])

    _native.project_add_provider(project, "primary", host_facts)
    _native.project_scan_builtin_provider(
        project, "syntax:tree-sitter:python", "augment"
    )
    _native.project_finalize_providers(project)
    mapped = json.loads(_native.project_map(project))
    if any(row["severity"] == "error" for row in mapped["diagnostics"]):
        raise AssertionError(mapped["diagnostics"])
    symbols = {
        symbol["name"]
        for file in mapped["files"]
        for symbol in file["symbols"]
    }
    if not {"Model", "Model.run", "train"}.issubset(symbols):
        raise AssertionError(f"portable syntax evidence was lost: {sorted(symbols)!r}")
    print("optional host syntax applicability passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
