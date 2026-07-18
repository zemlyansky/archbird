#!/usr/bin/env python3
"""Host syntax rejection must not poison successful portable evidence."""

from __future__ import annotations

import hashlib
import json

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
    raw = SOURCE.encode()
    path = "pkg/portable.py"
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
                "root": ".",
                "schema_version": 1,
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
