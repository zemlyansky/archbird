#!/usr/bin/env python3
"""Exact ESM/CommonJS evidence and cross-file package-surface regressions."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_ecmascript_modules.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    sources = {
        "cjs/index.js": b"""
const api = {create};
Object.defineProperties(api, {Tensor: {get() { return T; }}});
api.stats = stats;
module.exports = api;
""",
        "cjs/package.json": b'{"name":"cjs","version":"1.0.0"}',
        "callable/index.js": b"""
class PPO {}
if (typeof module !== 'undefined') module.exports = PPO;
""",
        "callable/package.json": b'{"name":"callable","version":"1.0.0"}',
        "bridge/index.js": b"module.exports = require('./target.sync');\n",
        "bridge/target.sync.js": b"module.exports = {run, stop};\n",
        "bridge/target.sync.py": b"run = 'wrong language candidate'\n",
        "bridge/package.json": b'{"name":"bridge","version":"1.0.0"}',
        "esm/index.js": b"""
import base, {helper as localHelper} from './feature.js';
export default base;
export {feature as renamed} from './feature.js';
export * from './star.js';
""",
        "esm/feature.js": b"export const feature = 1; export default 2;\n",
        "esm/star.js": b"export const alpha = 1; export function beta() {} export default 3;\n",
        "esm/package.json": b'{"name":"esm","version":"1.0.0"}',
        "dynamic/index.js": b"""
const api = {};
api[key] = value;
module.exports = api;
""",
        "dynamic/package.json": b'{"name":"dynamic","version":"1.0.0"}',
        "descriptor/base.js": b"module.exports = {Tensor, nn, create};\n",
        "descriptor/index.js": b"""
const main = require('./base');
const api = {};
Object.defineProperties(api, Object.getOwnPropertyDescriptors(main));
api.create = main.createAsync;
module.exports = api;
""",
        "descriptor/package.json": b'{"name":"descriptor","version":"1.0.0"}',
        "namespace/base.js": b"export const alpha = 1; export function beta() {}\n",
        "namespace/index.js": b"""
import * as main from './base.js';
const api = {};
Object.defineProperties(api, Object.getOwnPropertyDescriptors(main));
module.exports = api;
""",
        "namespace/package.json": b'{"name":"namespace","version":"1.0.0"}',
        "quoted/index.js": b"""
const component = {watch: {'output._messages': {handler() { return 1; }}}};
""",
        "descriptor-dynamic/index.js": b"""
const api = {};
Object.defineProperties(api, descriptors());
module.exports = api;
""",
        "descriptor-dynamic/package.json": b'{"name":"descriptor-dynamic","version":"1.0.0"}',
        "directory/lib/index.js": b"module.exports = {resolved};\n",
        "directory/package.json": b'{"name":"directory","version":"1.0.0","main":"lib"}',
        "conditions/runtime.js": b"export const runtime = 1;\n",
        "conditions/types.js": b"export const typeOnly = 1;\n",
        "conditions/types-new.js": b"export const typeOnlyNew = 1;\n",
        "conditions/legacy.js": b"export const legacy = 1;\n",
        "conditions/cli.js": b"export const cliOnly = 1;\n",
        "conditions/package.json": b'''{
          "name":"conditions","version":"1.0.0",
          "main":"./legacy.js",
          "bin":{"conditions":"./cli.js"},
          "exports":{".":{
            "types":"./types.js",
            "types@>=5.2":"./types-new.js",
            "import":"./runtime.js"
          }}
        }''',
        "multi/a.js": b"export {first as shared} from './values.js';\n",
        "multi/b.js": b"export {second as shared} from './values.js';\n",
        "multi/values.js": b"export const first = 1; export const second = 2;\n",
        "multi/package.json": b'{"name":"multi","version":"1.0.0"}',
        "method/response.js": b"""
module.exports = {
  set(name, value) { return [name, value]; }
};
""",
        "getter/index.js": b"""
const req = {};
defineGetter(req, 'fresh', function () { return true; });
module.exports = req;
function defineGetter(object, name, getter) {
  Object.defineProperty(object, name, {
    configurable: true,
    get: getter
  });
}
""",
        "getter/package.json": b'{"name":"getter","version":"1.0.0"}',
        "getter-negative/index.js": b"""
const req = {};
register(req, 'fresh', function () { return true; });
module.exports = req;
function register(object, name, getter) {
  consume(object, name, getter);
}
""",
        "getter-conditional/index.js": b"""
const req = {};
defineGetter(req, 'fresh', function () { return true; });
function defineGetter(object, name, getter) {
  if (enabled) {
    Object.defineProperty(object, name, {get: getter});
  }
}
""",
        "getter-rebound/index.js": b"""
const req = {};
defineGetter(req, 'fresh', function () { return true; });
function defineGetter(object, name, getter) {
  name = chooseName();
  Object.defineProperty(object, name, {get: getter});
}
""",
        "getter-shadowed/index.js": b"""
const req = {};
defineGetter(req, 'fresh', function () { return true; });
function defineGetter(object, name, getter, Object) {
  Object.defineProperty(object, name, {get: getter});
}
""",
        "descriptor-value/index.js": b"""
const req = {};
defineValue(req, 'fresh', function () { return true; });
function defineValue(object, name, value) {
  Object.defineProperty(object, name, {value: value});
}
""",
        "tests/set.test.js": b"""
test('ctx.set should retain values', () => {
  const ctx = makeContext();
  ctx.set('x-test', 'value');
});
""",
        "tests/other.test.js": b"""
test('unrelated method candidate', () => {
  const value = makeValue();
  value.set('x-test', 'value');
});
""",
        "tests/callable.test.js": b"""
const PPO = require('../callable');
test('constructs PPO', () => { new PPO(); });
""",
    }
    files = []
    for path, raw in sorted(sources.items()):
        row = {
            "bytes": len(raw),
            "path": path,
            "roles": (
                ["manifest"]
                if path.endswith("package.json")
                else ["source", *(["test"] if path.startswith("tests/") else [])]
            ),
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
        if path.endswith(".js"):
            row.update(language="javascript", layer="javascript")
        files.append(row)
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": files,
        "producer": {
            "implementation_sha256": "4" * 64,
            "name": "ecmascript-module-test",
            "version": "1",
        },
        "project": "ecmascript-modules",
        "schema_version": 1,
    }
    config = {
        "schema_version": 1,
        "project": "ecmascript-modules",
        "layers": [
            {
                "name": "javascript",
                "role": "frontend",
                "language": "javascript",
                "globs": ["**/*.js"],
            }
        ],
        "packages": [
            {
                "name": name,
                "kind": "npm",
                "path": f"{name}/package.json",
                "layer": "javascript",
                "entries": [f"{name}/index.js"],
            }
            for name in (
                "bridge",
                "callable",
                "cjs",
                "descriptor",
                "descriptor-dynamic",
                "dynamic",
                "esm",
                "getter",
                "namespace",
            )
        ],
        "tests": [
            {
                "name": "javascript",
                "language": "javascript",
                "globs": ["tests/**/*.js"],
                "route_to": ["javascript"],
            }
        ],
    }
    config["packages"].append(
        {
            "name": "directory",
            "kind": "npm",
            "path": "directory/package.json",
            "layer": "javascript",
        }
    )
    config["packages"].append(
        {
            "name": "multi",
            "kind": "npm",
            "path": "multi/package.json",
            "layer": "javascript",
            "entries": ["multi/a.js", "multi/b.js"],
        }
    )
    config["packages"].append(
        {
            "name": "conditions",
            "kind": "npm",
            "path": "conditions/package.json",
            "layer": "javascript",
        }
    )
    project = extension.project_create(canonical(manifest))
    for path, raw in sorted(sources.items()):
        extension.project_add_source(project, path, raw)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_scan_builtin_provider(
        project, "syntax:tree-sitter:javascript", "primary"
    )
    syntax_facts = []
    for index in range(extension.project_counts(project)["providers"]):
        bundle = json.loads(extension.project_provider_facts(project, index))
        if bundle["producer"]["name"] == "archbird-tree-sitter-javascript":
            syntax_facts.extend(bundle["facts"])
    extension.project_scan_builtin_provider(
        project, "lexical:javascript", "augment"
    )
    extension.project_finalize_providers(project)
    mapped = json.loads(extension.project_map(project))

    quoted_file = next(row for row in mapped["files"] if row["path"] == "quoted/index.js")
    assert [row["name"] for row in quoted_file["symbols"]] == [
        "component.watch.'output._messages'.handler"
    ], quoted_file["symbols"]

    by_package = {row["name"]: row for row in mapped["packages"]}
    assert by_package["cjs"]["exports"] == ["Tensor", "create", "stats"], by_package
    assert by_package["callable"]["exports"] == ["default"], by_package
    callable_surface = by_package["callable"]["entrypoint_surfaces"][0]
    assert callable_surface["evidence_state"] == "partial", callable_surface
    assert callable_surface["export_origins"] == {
        "default": ["callable/index.js"]
    }, callable_surface
    assert by_package["bridge"]["exports"] == ["run", "stop"], by_package
    assert by_package["descriptor"]["exports"] == [
        "Tensor",
        "create",
        "nn",
    ], by_package
    assert by_package["namespace"]["exports"] == ["alpha", "beta"], by_package
    descriptor_surface = {
        row["path"]: row for row in by_package["descriptor"]["entrypoint_surfaces"]
    }["descriptor/index.js"]
    assert descriptor_surface["evidence_state"] == "complete", descriptor_surface
    assert descriptor_surface["exports"] == ["Tensor", "create", "nn"], descriptor_surface
    assert descriptor_surface["export_origins"] == {
        "Tensor": ["descriptor/base.js"],
        "create": ["descriptor/base.js", "descriptor/index.js"],
        "nn": ["descriptor/base.js"],
    }, descriptor_surface
    dynamic_surface = by_package["descriptor-dynamic"]["entrypoint_surfaces"][0]
    assert dynamic_surface["evidence_state"] == "partial", dynamic_surface
    assert by_package["directory"]["exports"] == ["resolved"], by_package
    assert by_package["conditions"]["exports"] == ["runtime"], by_package
    assert by_package["esm"]["exports"] == [
        "alpha",
        "beta",
        "default",
        "renamed",
    ], by_package
    assert "api" not in by_package["cjs"]["exports"]
    assert "require" not in by_package["bridge"]["exports"]
    assert by_package["multi"]["exports"] == ["shared"]
    assert by_package["multi"]["export_origins"]["shared"] == [
        "multi/values.js"
    ]
    assert by_package["bridge"]["export_origins"] == {
        "run": ["bridge/target.sync.js"],
        "stop": ["bridge/target.sync.js"],
    }
    assert by_package["esm"]["export_origins"]["renamed"] == [
        "esm/feature.js"
    ]
    assert by_package["esm"]["export_origins"]["alpha"] == ["esm/star.js"]

    facts = syntax_facts
    imported = [row for row in facts if row["domain"] == "imported-names"]
    assert sorted(
        (row["attributes"]["imported"], row["attributes"]["local"])
        for row in imported
    ) == [
        ("*", "main"),
        ("default", "base"),
        ("helper", "localHelper"),
    ]
    routes = [
        (row["kind"], row["name"])
        for row in facts
        if row["domain"] == "module-reexports"
    ]
    assert sorted(routes) == [
        ("commonjs-require", "./base"),
        ("commonjs-require", "./base.js"),
        ("commonjs-require", "./target.sync"),
        ("esm-star", "./star.js"),
    ], routes
    assert any(
        row["code"] == "package-export-surface-partial"
        and row["path"] == "dynamic/index.js"
        for row in mapped["diagnostics"]
    ), mapped["diagnostics"]
    assert any(
        row["code"] == "package-export-surface-partial"
        and row["path"] == "descriptor-dynamic/index.js"
        for row in mapped["diagnostics"]
    ), mapped["diagnostics"]
    assert not any(
        row["code"] == "package-export-surface-partial"
        and row["path"] in {"descriptor/index.js", "namespace/index.js"}
        for row in mapped["diagnostics"]
    ), mapped["diagnostics"]
    assert any(
        row["code"] == "package-export-origin-ambiguous"
        and row["path"] in {"multi/a.js", "multi/b.js"}
        for row in mapped["diagnostics"]
    ), mapped["diagnostics"]
    method_case = next(
        case
        for test in mapped["tests"]
        if test["path"] == "tests/set.test.js"
        for case in test["cases"]
        if case["selector"] == "ctx.set should retain values"
    )
    method_candidates = [
        row
        for row in method_case["route_evidence"]
        if row["relation"] == "method-name-candidate"
    ]
    assert len(method_candidates) == 1, method_case
    assert method_candidates[0]["target"] == "method/response.js", method_case
    assert method_candidates[0]["target_symbol"] == "set", method_case
    method_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["method/response.js:set"],
                    "direction": "upstream",
                    "depth": 0,
                    "test_depth": 0,
                }
            ),
        )
    )
    method_match = next(
        row
        for row in method_query["test_matches"]
        if row["path"] == "tests/set.test.js"
    )
    assert method_match["classification"] == "candidate", method_match
    assert method_match["evidence_scope"] == "case", method_match
    assert method_match["target"] == {
        "path": "method/response.js",
        "symbol": "set",
    }, method_match
    assert method_query["test_matches"][0]["path"] == "tests/set.test.js", (
        method_query["test_matches"]
    )
    assert method_query["test_matches"][0]["ranking_affinity"] == 16, method_match
    assert method_query["test_matches"][1]["ranking_affinity"] == 0, (
        method_query["test_matches"]
    )
    file_scoped_calls = [
        row for row in mapped["symbol_calls"] if "scope" in row["source"]
    ]
    assert file_scoped_calls, mapped["symbol_calls"]
    assert all(row["candidates"] for row in file_scoped_calls), file_scoped_calls
    callable_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["callable/index.js:PPO"],
                    "direction": "upstream",
                    "depth": 1,
                    "test_depth": 1,
                }
            ),
        )
    )
    assert callable_query["query"]["seed_identities"] == [
        {
            "kind": "symbol",
            "line": 2,
            "path": "callable/index.js",
            "symbol": "PPO",
        }
    ], callable_query["query"]
    assert any(
        row["source"] == {"path": "tests/callable.test.js", "scope": "test-file"}
        and any(
            candidate["path"] == "callable/index.js"
            and candidate["symbol"] == "PPO"
            for candidate in row["candidates"]
        )
        for row in callable_query["symbol_calls"]
    ), callable_query["symbol_calls"]
    assert any(
        row["path"] == "tests/callable.test.js"
        for row in callable_query["files"]
    ), callable_query["files"]
    callable_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["callable/index.js:PPO"],
                "direction": "upstream",
                "depth": 1,
                "test_depth": 1,
            }
        ),
    ).decode()
    for expected in (
        "symbol-seeds=1; file-seeds=0; selected-files=2",
        "symbol-local relations:",
        "tests/callable.test.js [test-file] --PPO",
        "file fallback (selected files; not proof of focus-symbol use):",
    ):
        assert expected in callable_report, callable_report
    candidate_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["method/response.js:set"],
                    "direction": "upstream",
                    "depth": 0,
                    "test_depth": 0,
                    "context": {"candidate": "collapse"},
                }
            ),
        )
    )
    assert candidate_query["test_matches"] == method_query["test_matches"], (
        candidate_query["test_matches"]
    )
    candidate_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["method/response.js:set"],
                "direction": "upstream",
                "depth": 0,
                "test_depth": 0,
                "context": {"candidate": "collapse"},
            }
        ),
    ).decode()
    assert "candidate-collapsed=2 conservative-collapsed=0" in candidate_report
    assert "tests/set.test.js:3:ctx.set should retain values" not in candidate_report
    assert (
        "2 candidate routes collapsed "
        "[provenance=derived; scope=case; target-role=requested-symbol; "
        "seed-distance=0; target=method/response.js:set; "
        "top=tests/set.test.js:2:ctx.set should retain values]"
        in candidate_report
    ), candidate_report
    assert "--candidate expand" in candidate_report
    getter_file = next(
        row for row in mapped["files"] if row["path"] == "getter/index.js"
    )
    assert [
        (row["name"], row["kind"])
        for row in getter_file["symbols"]
        if row["name"] == "req.fresh"
    ] == [("req.fresh", "method")], getter_file["symbols"]
    for negative_path in (
        "getter-negative/index.js",
        "getter-conditional/index.js",
        "getter-rebound/index.js",
        "getter-shadowed/index.js",
        "descriptor-value/index.js",
    ):
        getter_negative = next(
            row for row in mapped["files"] if row["path"] == negative_path
        )
        assert all(
            row["name"] != "req.fresh" for row in getter_negative["symbols"]
        ), (negative_path, getter_negative["symbols"])
    print("ECMAScript ESM/CommonJS package surfaces passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
