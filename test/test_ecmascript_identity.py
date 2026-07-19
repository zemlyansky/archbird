#!/usr/bin/env python3
"""Cross-provider ECMAScript symbol-identity contract regressions."""

from __future__ import annotations

import copy
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


SOURCES = {
    "js/factory.js": b"""
export function jsFactory() {
  class Product { run() {} }
  const Bound = class Internal { bound() {} };
  let Rebound; Rebound = class PrivateBound { rebound() {} };
  return class Returned { returned() {} };
}
export function jsOther() { class Product { run() {} } return Product; }
class Top { method() { class Local { nested() {} } return Local; } }
let Assigned; Assigned = class Private { assigned() {} };
export { jsFactory as makeJs };
""",
    "js/commonjs.js": b"""
function cjsFactory() { class Product { run() {} } return Product; }
module.exports = { create: cjsFactory };
""",
    "ts/factory.ts": b"""
interface Service {}
export function tsFactory() {
  class Product implements Service { run() {} }
  const Bound = class Internal implements Service { bound() {} };
  return class Returned implements Service { returned() {} };
}
""",
    "tsx/factory.tsx": b"""
type Props = {value: number};
export function tsxFactory(props: Props) {
  class Product { render() { return <span>{props.value}</span>; } }
  return Product;
}
""",
}

REQUIRED_BY_PATH = {
    "js/factory.js": [
        "Assigned",
        "Assigned.assigned",
        "Top.method.Local",
        "Top.method.Local.nested",
        "jsFactory.Bound",
        "jsFactory.Bound.bound",
        "jsFactory.Product",
        "jsFactory.Product.run",
        "jsFactory.Rebound",
        "jsFactory.Rebound.rebound",
        "jsFactory.Returned",
        "jsFactory.Returned.returned",
        "jsOther.Product",
        "jsOther.Product.run",
    ],
    "js/commonjs.js": ["cjsFactory.Product", "cjsFactory.Product.run"],
    "ts/factory.ts": [
        "tsFactory.Bound",
        "tsFactory.Bound.bound",
        "tsFactory.Product",
        "tsFactory.Product.run",
        "tsFactory.Returned",
        "tsFactory.Returned.returned",
    ],
    "tsx/factory.tsx": ["tsxFactory.Product", "tsxFactory.Product.render"],
}

REQUIRED = {
    "javascript": sorted(
        REQUIRED_BY_PATH["js/factory.js"] + REQUIRED_BY_PATH["js/commonjs.js"]
    ),
    "typescript": REQUIRED_BY_PATH["ts/factory.ts"],
    "tsx": REQUIRED_BY_PATH["tsx/factory.tsx"],
}


def build_project(extension, sources: dict[str, bytes]):
    files = []
    for path, raw in sorted(sources.items()):
        language = "typescript" if path.endswith(".tsx") else (
            "typescript" if path.endswith(".ts") else "javascript"
        )
        layer = "tsx" if path.endswith(".tsx") else language
        files.append(
            {
                "bytes": len(raw),
                "language": language,
                "layer": layer,
                "path": path,
                "roles": ["source"],
                "sha256": hashlib.sha256(raw).hexdigest(),
            }
        )
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": files,
        "producer": {
            "implementation_sha256": "9" * 64,
            "name": "ecmascript-identity-test",
            "version": "1",
        },
        "project": "ecmascript-identity",
        "schema_version": 1,
    }
    config = {
        "schema_version": 1,
        "project": "ecmascript-identity",
        "layers": [
            {
                "name": language,
                "language": "typescript" if language == "tsx" else language,
                "globs": [f"{folder}/**"],
            }
            for language, folder in (
                ("javascript", "js"),
                ("typescript", "ts"),
                ("tsx", "tsx"),
            )
        ],
        "checks": {
            "symbols": [
                {"layer": layer, "name": name}
                for layer, names in REQUIRED.items()
                for name in names
            ]
        },
    }
    project = extension.project_create(canonical(manifest))
    for path, raw in sorted(sources.items()):
        extension.project_add_source(project, path, raw)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_scan_builtin_provider(
        project, "lexical:javascript", "augment"
    )
    for language in ("javascript", "typescript", "tsx"):
        extension.project_scan_builtin_provider(
            project, f"syntax:tree-sitter:{language}", "primary"
        )
    providers = [
        json.loads(extension.project_provider_facts(project, index))
        for index in range(extension.project_counts(project)["providers"])
    ]
    extension.project_finalize_providers(project)
    return project, manifest, providers, json.loads(extension.project_map(project))


def check_providers(providers: list[dict]) -> None:
    lexical_facts = [
        fact
        for row in providers
        if row["producer"]["name"] == "archbird-native-js-lexical"
        for fact in row["facts"]
    ]
    syntax = [
        row
        for row in providers
        if row["producer"]["name"].startswith("archbird-tree-sitter-")
    ]

    def syntax_names(language: str, path: str) -> set[str]:
        return {
            fact["name"]
            for provider in syntax
            if provider["producer"]["name"]
            == f"archbird-tree-sitter-{language}"
            for fact in provider["facts"]
            if fact["domain"] == "symbols" and fact["path"] == path
        }
    weak = next(
        row
        for row in lexical_facts
        if row["domain"] == "symbols"
        and row["path"] == "js/factory.js"
        and row["name"] == "Product"
    )
    assert weak["attributes"]["identity_state"] == "partial", weak
    assert "jsFactory.Product" in syntax_names("javascript", "js/factory.js")
    assert "jsOther.Product" in syntax_names("javascript", "js/factory.js")
    assert "Internal" not in syntax_names("javascript", "js/factory.js")
    bound = next(
        row
        for provider in syntax
        if provider["producer"]["name"] == "archbird-tree-sitter-javascript"
        for row in provider["facts"]
        if row["domain"] == "symbols" and row["name"] == "jsFactory.Bound"
    )
    assert bound["attributes"]["internal_name"] == "Internal", bound
    assert "tsFactory.Product" in syntax_names("typescript", "ts/factory.ts")
    assert "tsxFactory.Product" in syntax_names("tsx", "tsx/factory.tsx")
    exports = {
        (row["path"], row["name"]): row["attributes"]
        for provider in syntax
        for row in provider["facts"]
        if row["domain"] == "exports"
    }
    assert exports[("js/factory.js", "makeJs")]["origin_name"] == "jsFactory"
    assert exports[("js/commonjs.js", "create")]["origin_name"] == "cjsFactory"


def check_map(extension, mapped: dict) -> None:
    diagnostics = [
        row for row in mapped["diagnostics"] if row["severity"] == "error"
    ]
    assert diagnostics == [], diagnostics
    by_path = {row["path"]: row for row in mapped["files"]}
    for path, required in REQUIRED_BY_PATH.items():
        actual = {row["name"] for row in by_path[path]["symbols"]}
        assert set(required) <= actual, (path, sorted(actual))
    assert len(
        [
            row
            for row in by_path["js/factory.js"]["symbols"]
            if row["name"].endswith(".Product")
        ]
    ) == 2

    request = canonical(
        {
            "symbols": ["js/factory.js:jsFactory.Product"],
            "direction": "both",
            "depth": 1,
            "test_depth": 1,
        }
    )
    query = json.loads(extension.map_query(canonical(mapped), request))
    assert [
        (row["path"], row["name"], row["kind"])
        for row in query["matched_symbols"]
    ] == [("js/factory.js", "jsFactory.Product", "class")], query
    assert query["query"]["seed_identities"] == [
        {
            "kind": "symbol",
            "line": 3,
            "path": "js/factory.js",
            "symbol": "jsFactory.Product",
        }
    ], query["query"]
    report = extension.map_query_markdown(canonical(mapped), request).decode()
    assert "symbol-seeds=1; file-seeds=0" in report, report
    assert "jsFactory.Product" in report, report

    for view in range(3):
        report = extension.map_markdown_view(
            canonical(mapped), view, 2, 0
        ).decode()
        assert "jsFactory.Product" in report, (view, report)


def check_diff(extension, before: dict) -> None:
    changed = dict(SOURCES)
    changed["js/factory.js"] = changed["js/factory.js"].replace(
        b"class Product { run() {} }", b"class Renamed { run() {} }", 1
    )
    _, _, _, after = build_project(extension, changed)
    diff = json.loads(extension.map_diff(canonical(before), canonical(after)))
    rendered = json.dumps(diff, sort_keys=True)
    assert "jsFactory.Product" in rendered, diff
    assert "jsFactory.Renamed" in rendered, diff


def check_verify(extension, mapped: dict) -> None:
    expected = sorted(REQUIRED["javascript"])
    suite = {
        "schema_version": 1,
        "suite": "ecmascript-symbol-identity",
        "projects": {"subject": {"map": "subject.map.json"}},
        "extractors": {
            "required.javascript": {"kind": "literal_set", "values": expected},
            "actual.javascript": {
                "kind": "symbols",
                "project": "subject",
                "layer": "javascript",
            },
        },
        "checks": [
            {
                "id": "ECMASCRIPT-NESTED-IDENTITY",
                "assert": "required_subset",
                "expected": "required.javascript",
                "actual": "actual.javascript",
                "severity": "error",
                "owner": "evidence",
                "rationale": "Nested declaration identity must remain qualified.",
            }
        ],
    }
    verify_input = {
        "schema_version": 1,
        "artifact": "verification-input",
        "suite_path": "ecmascript-identity.verify.json",
        "projects": [
            {
                "name": "subject",
                "map": mapped,
                "sources": [
                    {"path": path, "text": raw.decode()}
                    for path, raw in sorted(SOURCES.items())
                ],
            }
        ],
        "provided_facts": [],
        "attestations": [],
        "baseline": None,
    }
    verified = json.loads(
        extension.verification_analyze(canonical(suite), canonical(verify_input))
    )
    check = verified["checks"][0]
    assert check["status"] == "pass", check

    broken = copy.deepcopy(mapped)
    js_file = next(row for row in broken["files"] if row["path"] == "js/factory.js")
    js_file["symbols"] = [
        row for row in js_file["symbols"] if row["name"] != "jsFactory.Product"
    ]
    broken["evidence"]["input_sha256"] = "a" * 64
    verify_input["projects"][0]["map"] = broken
    failed = json.loads(
        extension.verification_analyze(canonical(suite), canonical(verify_input))
    )
    failed_check = failed["checks"][0]
    assert failed_check["status"] == "fail", failed_check
    proposal = json.loads(
        extension.change_proposal(
            canonical(failed), failed_check["findings"][0]["fingerprint"]
        )
    )
    assert proposal["origin"]["check"] == "ECMASCRIPT-NESTED-IDENTITY", proposal


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_ecmascript_identity.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    project, _, providers, mapped = build_project(extension, SOURCES)
    del project
    check_providers(providers)
    check_map(extension, mapped)
    check_diff(extension, mapped)
    check_verify(extension, mapped)
    print("ECMAScript symbol identity contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
