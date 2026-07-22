#!/usr/bin/env python3
"""Differential tests for CPython AST provider -> native typed fact store."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.scanners import python_file_facts


SOURCE = '''\
from .api import Tensor as T, helper
import os.path as osp

VALUE = T

class Model(T):
    def run(self, x: int = helper()) -> int:
        return self.forward(x) + local(x)

async def train(model, item):
    return await model.run(item)

def local(value):
    return max(value, 0)

__all__ = ["Model", "train", "VALUE", "T"]
'''


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
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def projection(row: dict) -> tuple:
    return (
        [
            (
                symbol["line"],
                symbol["name"],
                symbol["kind"],
                symbol["scope"],
                symbol["signature"],
            )
            for symbol in row["symbols"]
        ],
        set(row["calls"]),
        row["call_counts"],
        set(row["method_calls"]),
        row["method_call_counts"],
        set(row["imports"]),
        {key: set(value) for key, value in row["imported_names"].items()},
        set(row["exports"]),
        row["export_origins"],
        set(row["reexport_candidates"]),
    )


def verify_binding(extension) -> None:
    config = {
        "schema_version": 2,
        "project": "python-binding",
        "layers": [
            {
                "name": "python",
                "language": "python",
                "globs": ["**/*.py"],
                "required": False,
            }
        ],
        "constraints": {
            "PY-BINDING": {
                "assert": "set_equal",
                "expected": {"literal": ["A"]},
                "actual": {"literal": ["A"]},
                "owner": "test",
                "rationale": "Exercise the public CPython constraint binding.",
            }
        },
    }
    compiled = json.loads(extension.project_configuration_compile(canonical(config)))
    map_document = {
        "artifact": "map",
        "schema_version": 6,
        "project": "python-binding",
        "evidence": {
            "config_sha256": compiled["map_config_sha256"],
            "input_sha256": "2" * 64,
        },
        "tool": {
            "name": "archbird",
            "version": "fixture",
            "implementation_sha256": "3" * 64,
        },
        "diagnostics": [],
    }
    result = json.loads(
        extension.constraints_evaluate(
            canonical(config), canonical(map_document)
        )
    )
    if (
        result["artifact"] != "verification"
        or result["schema_version"] != 2
        or result["constraints"][0]["status"] != "pass"
    ):
        raise AssertionError("public CPython constraint binding failed")


def oracle_projection(facts) -> tuple:
    return (
        [
            (row.line, row.name, row.kind, row.scope, row.signature)
            for row in facts.symbols
        ],
        facts.calls,
        facts.call_counts,
        facts.method_calls,
        facts.method_call_counts,
        facts.imports,
        facts.imported_names,
        facts.exports,
        facts.export_origins,
        facts.reexport_candidates,
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_python_frontend.py EXTENSION REPOSITORY_ROOT")
    extension = load_extension(Path(sys.argv[1]).resolve())
    verify_binding(extension)
    root = Path(sys.argv[2]).resolve()
    provider = load_provider(root)
    raw = SOURCE.encode()
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(raw),
                "language": "python",
                "layer": "py",
                "path": "pkg/__init__.py",
                "roles": ["source"],
                "sha256": __import__("hashlib").sha256(raw).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "python-test-host",
            "version": "1",
        },
        "project": "python-test",
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    extension.project_add_source(project, "pkg/__init__.py", raw)
    extension.project_finalize_sources(project)
    provider_json = provider.python_ast_provider_facts(
        project="python-test",
        path="pkg/__init__.py",
        text=SOURCE,
        source_manifest_sha256=extension.project_manifest_sha256(project),
    )
    extension.project_add_provider(project, "primary", provider_json)
    extension.project_finalize_providers(project)
    native = json.loads(extension.project_file_facts(project))["files"][0]
    oracle = python_file_facts(
        "pkg/__init__.py", "py", SOURCE, __import__("hashlib").sha256(raw).hexdigest()
    )
    expected = list(oracle_projection(oracle))
    expected_symbols = list(expected[0])
    run_index = next(
        index for index, row in enumerate(expected_symbols) if row[1] == "Model.run"
    )
    assert expected_symbols[run_index][4] == "def run(self, x: int=helper())"
    expected_symbols[run_index] = (
        *expected_symbols[run_index][:4],
        "def run(self, x: int=helper()) -> int",
    )
    expected[0] = expected_symbols
    expected_projection = tuple(expected)
    if projection(native) != expected_projection:
        raise AssertionError(
            f"Python provider projection differs\nnative={projection(native)!r}"
            f"\nexpected={expected_projection!r}"
        )
    if extension.project_provider_facts(project, 0) != provider_json:
        raise AssertionError("typed provider roundtrip is not byte-identical")
    provider_document = json.loads(provider_json)
    domain_counts: dict[str, int] = {}
    for fact in provider_document["facts"]:
        domain_counts[fact["domain"]] = domain_counts.get(fact["domain"], 0) + 1
    expected_domains = {
        "calls": 3,
        "export-origins": 4,
        "exports": 4,
        "imported-name-groups": 1,
        "imported-names": 2,
        "imports": 2,
        "method-calls": 2,
        "module-bindings": 1,
        "name-uses": 3,
        "reexport-candidates": 3,
        "symbols": 4,
    }
    if domain_counts != expected_domains:
        raise AssertionError(domain_counts)
    if extension.project_counts(project) != {
        "sources": 1,
        "providers": 1,
        "facts": len(provider_document["facts"]),
    }:
        raise AssertionError(extension.project_counts(project))

    multiline_call = "result = (\n    document\n).encode('utf-8')\n"
    multiline_bundle = json.loads(
        provider.python_ast_provider_facts(
            project="python-test",
            path="pkg/multiline.py",
            text=multiline_call,
            source_manifest_sha256="0" * 64,
        )
    )
    multiline_fact = next(
        row
        for row in multiline_bundle["facts"]
        if row["domain"] == "method-calls" and row["name"] == "encode"
    )
    if multiline_fact["attributes"]["line"] != 3:
        raise AssertionError(
            "Python AST fact line must identify the exact member token, not "
            "the first line of its multiline Attribute node"
        )

    invalid = "def broken(:\n"
    bad = provider.python_ast_provider_facts(
        project="python-test",
        path="pkg/__init__.py",
        text=invalid,
        source_manifest_sha256="0" * 64,
    )
    bad_doc = json.loads(bad)
    if bad_doc["facts"] or not bad_doc["diagnostics"]:
        raise AssertionError("syntax failure must remain explicit provider evidence")
    if {row["coverage"] for row in bad_doc["capabilities"]} != {"none"}:
        raise AssertionError("syntax failure must not claim extracted coverage")
    if bad_doc["diagnostics"] != [
        {
            "code": "python-ast-inapplicable",
            "message": bad_doc["diagnostics"][0]["message"],
            "path": "pkg/__init__.py",
            "project": "python-test",
            "severity": "note",
            "span": {"end": 11, "start": 11},
        }
    ]:
        raise AssertionError(
            f"host grammar rejection was not explicit inapplicability: "
            f"{bad_doc['diagnostics']!r}"
        )

    encoded_raw = (
        b"# coding: iso-8859-1\n"
        b"label = 'caf\xe9'\n"
        b"def r\xe9sum\xe9():\n"
        b"    return label\n"
    )
    encoded_doc = json.loads(
        provider.python_ast_provider_facts(
            project="python-test",
            path="pkg/encoded.py",
            source_bytes=encoded_raw,
        )
    )
    encoded_symbol = next(
        row
        for row in encoded_doc["facts"]
        if row["domain"] == "symbols" and row.get("name") == "r\xe9sum\xe9"
    )
    encoded_start = encoded_raw.index(b"r\xe9sum\xe9")
    if encoded_symbol["span"] != {
        "end": encoded_start + len(b"r\xe9sum\xe9"),
        "start": encoded_start,
    }:
        raise AssertionError(
            f"encoded Python span was not mapped to source bytes: {encoded_symbol!r}"
        )
    if encoded_doc["inputs"][0]["source_sha256"] != __import__(
        "hashlib"
    ).sha256(encoded_raw).hexdigest():
        raise AssertionError("encoded Python provider did not bind exact source bytes")

    invalid_encoding_raw = b'value = "b\xf6se"\n'
    invalid_encoding_doc = json.loads(
        provider.python_ast_provider_facts(
            project="python-test",
            path="pkg/invalid_encoding.py",
            source_bytes=invalid_encoding_raw,
        )
    )
    if invalid_encoding_doc["facts"] or {
        row["coverage"] for row in invalid_encoding_doc["capabilities"]
    } != {"none"}:
        raise AssertionError("undecodable Python source claimed AST coverage")
    if [row["code"] for row in invalid_encoding_doc["diagnostics"]] != [
        "python-ast-encoding-inapplicable"
    ]:
        raise AssertionError(invalid_encoding_doc["diagnostics"])

    invalid_raw = invalid.encode()
    invalid_manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(invalid_raw),
                "language": "python",
                "layer": "py",
                "path": "pkg/broken.py",
                "roles": ["source"],
                "sha256": __import__("hashlib").sha256(invalid_raw).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "python-test-host",
            "version": "1",
        },
        "project": "python-error-test",
        "schema_version": 1,
    }
    invalid_project = extension.project_create(canonical(invalid_manifest))
    extension.project_add_source(invalid_project, "pkg/broken.py", invalid_raw)
    extension.project_finalize_sources(invalid_project)
    invalid_config = {
        "description": "provider diagnostic routing fixture",
        "layers": [
            {
                "globs": ["pkg/**/*.py"],
                "language": "python",
                "name": "py",
                "role": "core",
            }
        ],
        "project": "python-error-test",
        "schema_version": 2,
    }
    extension.project_set_config(invalid_project, canonical(invalid_config))
    extension.project_add_provider(
        invalid_project,
        "primary",
        provider.python_ast_provider_facts(
            project="python-error-test",
            path="pkg/broken.py",
            text=invalid,
            source_manifest_sha256=extension.project_manifest_sha256(
                invalid_project
            ),
        ),
    )
    extension.project_finalize_providers(invalid_project)
    invalid_map = json.loads(extension.project_map(invalid_project))
    if [row["code"] for row in invalid_map["diagnostics"]] != [
        "python-ast-inapplicable"
    ]:
        raise AssertionError("provider diagnostic was absent from aggregate Map")
    if invalid_map["diagnostics"][0]["severity"] != "note":
        raise AssertionError("optional host grammar rejection became blocking")

    # A host parser is an optional precision provider, not the arbiter of
    # whether repository source is valid.  Force a rejection of otherwise
    # portable source so the regression is independent of the test host's
    # CPython minor version, then prove Tree-sitter evidence survives the
    # common provider merge without a blocking diagnostic.
    original_parse = provider.ast.parse
    try:
        def reject_portable_source(*args, **kwargs):
            del args, kwargs
            raise SyntaxError("fixture host grammar is older", ("pkg/portable.py", 1, 1, "class Model:\n"))

        provider.ast.parse = reject_portable_source
        rejected_provider = provider.python_ast_provider_facts(
            project="python-portable",
            path="pkg/portable.py",
            text=SOURCE,
            source_manifest_sha256="0" * 64,
        )
    finally:
        provider.ast.parse = original_parse
    portable_raw = SOURCE.encode()
    portable_manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(portable_raw),
                "language": "python",
                "layer": "py",
                "path": "pkg/portable.py",
                "roles": ["source"],
                "sha256": __import__("hashlib").sha256(portable_raw).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "python-test-host",
            "version": "1",
        },
        "project": "python-portable",
        "schema_version": 1,
    }
    portable_project = extension.project_create(canonical(portable_manifest))
    extension.project_add_source(portable_project, "pkg/portable.py", portable_raw)
    extension.project_finalize_sources(portable_project)
    portable_config = {
        "description": "optional host parser applicability fixture",
        "layers": [
            {
                "globs": ["pkg/**/*.py"],
                "language": "python",
                "name": "py",
                "role": "core",
            }
        ],
        "project": "python-portable",
        "schema_version": 2,
    }
    extension.project_set_config(portable_project, canonical(portable_config))
    extension.project_add_provider(portable_project, "primary", rejected_provider)
    extension.project_scan_builtin_provider(
        portable_project, "syntax:tree-sitter:python", "augment"
    )
    extension.project_finalize_providers(portable_project)
    portable_map = json.loads(extension.project_map(portable_project))
    if any(row["severity"] == "error" for row in portable_map["diagnostics"]):
        raise AssertionError(
            f"optional host grammar rejection poisoned portable Map: "
            f"{portable_map['diagnostics']!r}"
        )
    portable_symbols = {
        symbol["name"]
        for row in portable_map["files"]
        for symbol in row["symbols"]
    }
    if not {"Model", "Model.run", "train", "local"}.issubset(portable_symbols):
        raise AssertionError(
            f"portable syntax facts did not survive host rejection: "
            f"{sorted(portable_symbols)!r}"
        )

    normalized_source = "class ass:\n    def Kelvin(self):\n        return self.Kelvin()\n"
    normalized = json.loads(
        provider.python_ast_provider_facts(
            project="python-test",
            path="pkg/__init__.py",
            text=normalized_source,
            source_manifest_sha256="0" * 64,
        )
    )
    raw_normalized = normalized_source.encode("utf-8")
    witnessed = {
        raw_normalized[row["span"]["start"] : row["span"]["end"]].decode(
            "utf-8"
        )
        for row in normalized["facts"]
        if row.get("name") in {"ass", "ass.Kelvin", "Kelvin"}
    }
    if witnessed != {"ass", "Kelvin"}:
        raise AssertionError(f"incorrect normalized identifier spans: {witnessed!r}")
    print("native Python frontend parity passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
