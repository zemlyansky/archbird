from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess

from archbird import _native
from archbird.errors import ConfigError
from archbird.native import (
    Project,
    Source,
    compile_project_configuration,
    compile_query_plan_json,
    diff_maps_json,
    evaluate_constraints_json,
    evaluate_projection_json,
    freeze_constraints_json,
    publish_okf_bundle,
    query_map_markdown,
    query_map_json,
    render_map_markdown,
)
from archbird.project_configuration import compile_ad_hoc_query, compile_named_query


ROOT = Path(__file__).resolve().parents[1]


def _assert_project_configuration_conformance() -> None:
    corpus = json.loads(
        (ROOT / "test/fixtures/project_configuration_conformance.json").read_text()
    )
    assert corpus["schema_version"] == 1
    for entry in corpus["cases"]:
        configuration = json.dumps(
            entry["configuration"], sort_keys=True, separators=(",", ":")
        ).encode()
        try:
            compile_project_configuration(configuration)
        except RuntimeError as error:
            if entry["valid"]:
                raise AssertionError(
                    f"native compiler rejected conformance case {entry['id']}: {error}"
                ) from error
        else:
            assert entry["valid"], (
                f"native compiler accepted invalid conformance case {entry['id']}"
            )
    matrix = next(
        entry
        for entry in corpus["cases"]
        if entry["id"] == "all-projection-operators"
    )
    for projection_id, projection in matrix["configuration"][
        "projections"
    ].items():
        foreign = (
            {"artifacts": ["library"]}
            if projection["select"] == "file_metrics"
            else {"metric": "bytes"}
        )
        mutated = {
            "project": "conformance",
            "layers": [
                {"name": "c", "language": "c", "globs": ["src/**/*.c"]}
            ],
            "projections": {projection_id: {**projection, **foreign}},
        }
        try:
            compile_project_configuration(json.dumps(mutated).encode())
        except RuntimeError:
            pass
        else:
            raise AssertionError(
                f"projection {projection_id} accepted an option owned by another operator"
            )
    constraints = next(
        entry for entry in corpus["cases"] if entry["id"] == "all-constraint-forms"
    )
    for constraint_id, constraint in constraints["configuration"][
        "constraints"
    ].items():
        foreign = (
            {"actual": {"literal": []}}
            if "kind" in constraint
            else {"bridge": "unused"}
        )
        mutated = {
            "project": "conformance",
            "layers": [
                {"name": "c", "language": "c", "globs": ["src/**/*.c"]}
            ],
            "constraints": {constraint_id: {**constraint, **foreign}},
        }
        try:
            compile_project_configuration(json.dumps(mutated).encode())
        except RuntimeError:
            pass
        else:
            discriminator = constraint.get("kind", constraint.get("assert"))
            raise AssertionError(
                f"constraint {discriminator} accepted an option owned by another contract"
            )


def _map(config: dict[str, object]) -> bytes:
    project = Project.from_repository(
        ROOT,
        config=json.dumps(config, sort_keys=True, separators=(",", ":")).encode(),
        jobs=1,
    )
    return project.map_json()


def _fixture_map(root: Path) -> bytes:
    project = Project.from_repository(
        root,
        config=(root / "archbird.json").read_bytes(),
        jobs=1,
    )
    return project.map_json()


def _assert_import_resolution_soundness() -> None:
    root = ROOT / "build/import-resolution-soundness"
    shutil.rmtree(root, ignore_errors=True)

    def evaluate(
        name: str,
        source: str,
        *,
        import_roots: list[str] | None = None,
        local_module: bool = False,
        declared_external: bool = False,
    ) -> tuple[dict, dict, dict, dict]:
        repository = root / name
        (repository / "app").mkdir(parents=True)
        (repository / "app/main.py").write_text(source, encoding="utf-8")
        if local_module:
            (repository / "ui").mkdir()
            (repository / "ui/widget.py").write_text(
                "def render():\n    return 'ok'\n", encoding="utf-8"
            )
        layer = {
            "globs": ["**/*.py"],
            "language": "python",
            "name": "python",
        }
        if import_roots is not None:
            layer["import_roots"] = import_roots
        configuration: dict[str, object] = {
            "project": name,
            "layers": [layer],
            "components": [
                {"name": "app", "paths": ["app/**"]},
                {"name": "ui", "paths": ["ui/**"]},
            ],
            "constraints": {
                "NO-IMPORT-EDGES": {
                    "edges": [],
                    "kind": "allowed_component_edges",
                    "kinds": ["import"],
                    "owner": "architecture",
                    "rationale": "Exercise honest import-resolution completeness.",
                }
            },
        }
        if declared_external:
            (repository / "pyproject.toml").write_text(
                "[project]\n"
                f'name = "{name}"\n'
                'version = "1.0.0"\n'
                'dependencies = ["requests>=2"]\n',
                encoding="utf-8",
            )
            configuration["packages"] = [
                {
                    "kind": "python",
                    "layer": "python",
                    "name": "python-package",
                    "path": "pyproject.toml",
                }
            ]
        encoded = json.dumps(
            configuration, sort_keys=True, separators=(",", ":")
        ).encode()
        project = Project.from_repository(repository, config=encoded, jobs=1)
        map_document = json.loads(project.map_json())
        file_edges = json.loads(
            evaluate_projection_json(
                json.dumps(map_document, separators=(",", ":")).encode(),
                {
                    "id": "file-imports",
                    "kinds": ["import"],
                    "select": "file_edges",
                },
            )
        )
        component_edges = json.loads(
            evaluate_projection_json(
                json.dumps(map_document, separators=(",", ":")).encode(),
                {
                    "id": "component-imports",
                    "kinds": ["import"],
                    "select": "component_edges",
                },
            )
        )
        verification = json.loads(
            evaluate_constraints_json(
                encoded,
                json.dumps(map_document, separators=(",", ":")).encode(),
            )
        )
        return map_document, file_edges, component_edges, verification

    unresolved_map, unresolved_files, unresolved_components, unresolved_verify = (
        evaluate(
            "unresolved",
            "from ui.widget import render\n\nrender()\n",
            local_module=True,
        )
    )
    assert unresolved_files["completeness"]["classification"] == "incomplete"
    assert unresolved_files["completeness"]["counts"]["unknown"] == 1
    assert (
        unresolved_components["completeness"]["classification"] == "incomplete"
    )
    assert unresolved_components["completeness"]["counts"]["unknown"] == 1
    assert unresolved_verify["constraints"][0]["status"] == "unknown"
    unresolved_diagnostics = [
        row
        for row in unresolved_map["diagnostics"]
        if row["code"] == "unresolved-import"
    ]
    assert len(unresolved_diagnostics) == 1
    assert unresolved_diagnostics[0]["path"] == "app/main.py"
    assert unresolved_diagnostics[0]["span"]["start"] >= 0
    assert "ui.widget" in unresolved_diagnostics[0]["message"]

    resolved_map, resolved_files, resolved_components, resolved_verify = evaluate(
        "resolved",
        "from ui.widget import render\n\nrender()\n",
        import_roots=["."],
        local_module=True,
    )
    assert resolved_files["completeness"]["classification"] == "complete"
    assert resolved_files["completeness"]["counts"]["unknown"] == 0
    assert resolved_components["completeness"]["classification"] == "complete"
    assert resolved_components["completeness"]["counts"]["unknown"] == 0
    assert resolved_verify["constraints"][0]["status"] == "fail"
    assert not any(
        row["code"] == "unresolved-import"
        for row in resolved_map["diagnostics"]
    )

    _, empty_files, empty_components, empty_verify = evaluate(
        "empty", "def main():\n    return 0\n"
    )
    assert empty_files["completeness"]["classification"] == "complete"
    assert empty_files["fact"]["items"] == []
    assert empty_components["completeness"]["classification"] == "complete"
    assert empty_components["fact"]["items"] == []
    assert empty_verify["constraints"][0]["status"] == "pass"

    external_map, external_files, external_components, external_verify = evaluate(
        "declared-external",
        "import requests\n",
        declared_external=True,
    )
    assert any(edge["kind"] == "external" for edge in external_map["edges"])
    assert external_files["completeness"]["classification"] == "complete"
    assert external_files["fact"]["items"] == []
    assert external_components["completeness"]["classification"] == "complete"
    assert external_components["fact"]["items"] == []
    assert external_verify["constraints"][0]["status"] == "pass"

    namespace = root / "namespace-package"
    (namespace / "app").mkdir(parents=True)
    (namespace / "tools").mkdir()
    (namespace / "app/main.py").write_text(
        "from tools import helper\n\nhelper.run()\n", encoding="utf-8"
    )
    (namespace / "tools/helper.py").write_text(
        "def run():\n    return 1\n", encoding="utf-8"
    )
    namespace_config = {
        "project": "namespace-package",
        "layers": [
            {
                "globs": ["**/*.py"],
                "import_roots": ["."],
                "language": "python",
                "name": "python",
            }
        ],
        "components": [
            {"name": "app", "paths": ["app/**"]},
            {"name": "tools", "paths": ["tools/**"]},
        ],
    }
    namespace_map = json.loads(
        Project.from_repository(
            namespace,
            config=json.dumps(namespace_config, separators=(",", ":")).encode(),
            jobs=1,
        ).map_json()
    )
    assert any(
        row["kind"] == "import"
        and row["source"] == "app/main.py"
        and row["target"] == "tools/helper.py"
        for row in namespace_map["edges"]
    )
    assert not any(
        row["code"] == "unresolved-import"
        for row in namespace_map["diagnostics"]
    )

    c_system = root / "c-system"
    (c_system / "src").mkdir(parents=True)
    (c_system / "src/main.c").write_text(
        "#include <stdio.h>\nint main(void) { return 0; }\n",
        encoding="utf-8",
    )
    c_system_config = {
        "project": "c-system",
        "layers": [
            {
                "globs": ["**/*.c"],
                "language": "c",
                "name": "c",
            }
        ],
        "components": [{"name": "app", "paths": ["src/**"]}],
    }
    c_system_map = json.loads(
        Project.from_repository(
            c_system,
            config=json.dumps(c_system_config, separators=(",", ":")).encode(),
            jobs=1,
        ).map_json()
    )
    assert any(
        row["kind"] == "external"
        and row["source"] == "src/main.c"
        and row["target"] == "package:stdio.h"
        for row in c_system_map["edges"]
    )
    assert not any(
        row["code"] == "unresolved-import"
        for row in c_system_map["diagnostics"]
    )

    c_template = root / "c-template"
    (c_template / "src").mkdir(parents=True)
    (c_template / "include").mkdir()
    (c_template / "src/main.c").write_text(
        '#include "generated.h"\nint main(void) { return GENERATED; }\n',
        encoding="utf-8",
    )
    (c_template / "include/generated.h.generic").write_text(
        "#define GENERATED 0\n", encoding="utf-8"
    )
    c_template_config = {
        "project": "c-template",
        "layers": [
            {
                "globs": ["src/*.c", "include/*.generic"],
                "import_roots": ["include"],
                "language": "c",
                "name": "c",
            }
        ],
        "components": [
            {"name": "app", "paths": ["src/**"]},
            {"name": "template", "paths": ["include/**"]},
        ],
    }
    c_template_map = json.loads(
        Project.from_repository(
            c_template,
            config=json.dumps(c_template_config, separators=(",", ":")).encode(),
            jobs=1,
        ).map_json()
    )
    assert any(
        row["kind"] == "import"
        and row["source"] == "src/main.c"
        and row["target"] == "include/generated.h.generic"
        for row in c_template_map["edges"]
    )
    assert not any(
        row["code"] == "unresolved-import"
        for row in c_template_map["diagnostics"]
    )


def _assert_partial_configuration_overlay() -> None:
    root = ROOT / "build/partial-configuration-overlay"
    shutil.rmtree(root, ignore_errors=True)
    (root / "src").mkdir(parents=True)
    (root / "tests").mkdir()
    (root / "src/core.py").write_text("def run():\n    return 1\n")
    (root / "tests/test_core.py").write_text(
        "from src.core import run\n\ndef test_run():\n    assert run() == 1\n"
    )
    (root / "pyproject.toml").write_text(
        "[project]\nname = \"partial-overlay\"\nversion = \"1.0.0\"\n"
    )
    partial = {
        "constraints": {
            "HAS-CORE": {
                "kind": "required_paths",
                "paths": ["src/core.py"],
                "owner": "architecture",
                "rationale": "The discovered core remains mapped.",
            }
        }
    }
    unconfigured = Project.from_repository(root, jobs=1).map_json()
    configured = Project.from_repository(
        root,
        config=json.dumps(partial, separators=(",", ":")).encode(),
        jobs=1,
    ).map_json()
    assert configured == unconfigured
    assert json.loads(
        evaluate_constraints_json(
            json.dumps(partial, separators=(",", ":")).encode(), configured
        )
    )["constraints"][0]["status"] == "pass"

    explicit = {
        **partial,
        "layers": [
            {
                "name": "owned",
                "language": "python",
                "globs": ["src/**"],
            }
        ],
    }
    explicit_map = json.loads(
        Project.from_repository(
            root,
            config=json.dumps(explicit, separators=(",", ":")).encode(),
            jobs=1,
        ).map_json()
    )
    assert [layer["name"] for layer in explicit_map["layers"]] == ["owned"]
    assert [source["path"] for source in explicit_map["files"]] == ["src/core.py"]
    assert [
        (package["name"], package["layer"])
        for package in explicit_map["packages"]
    ] == [("python-root", "owned")]

    completed = subprocess.run(
        [
            str(ROOT / "archbird"),
            "verify",
            "--root",
            str(root),
            "--config",
            "-",
            "--format",
            "json",
            "--check",
            "--progress",
            "never",
        ],
        input=json.dumps(partial, separators=(",", ":")).encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr.decode()
    assert json.loads(completed.stdout)["constraints"][0]["status"] == "pass"


def _assert_invalid_projection(
    map_document: dict[str, object], definition: dict[str, object]
) -> None:
    try:
        evaluate_projection_json(
            json.dumps(map_document, separators=(",", ":")).encode(), definition
        )
    except RuntimeError as error:
        assert "invalid" in str(error).lower()
    else:
        raise AssertionError("malformed Map inventory produced a projection")


def _assert_symbol_occurrence_projection() -> None:
    root = ROOT / "build/symbol-occurrence-projection"
    shutil.rmtree(root, ignore_errors=True)
    sources = {
        "c/api.c": '#include "api.h"\nint old_api(int value) { return value + 1; }\n',
        "c/api.h": "int old_api(int value);\n",
        "c/use.c": '#include "api.h"\nint result(void) { return old_api(1); }\n',
        "js/alias.js": (
            'import { oldApi as keptAlias } from "./api.js";\n'
            'export { oldApi as publicAlias } from "./api.js";\n'
            "export const result = keptAlias(2);\n"
        ),
        "js/api.js": "export function oldApi(value) { return value + 1; }\n",
        "js/use.js": (
            'import { oldApi } from "./api.js";\n'
            "export const result = oldApi(1);\n"
        ),
        "py/alias.py": (
            "from api import old_api as kept_alias\n"
            "result = kept_alias(2)\n"
        ),
        "py/api.py": "def old_api(value):\n    return value + 1\n",
        "py/nested.py": (
            "def outer():\n"
            "    def old_api(value):\n"
            "        return value + 1\n"
            "    return old_api(3)\n"
        ),
        "py/shadow.py": (
            "from api import old_api\n"
            "def local_use():\n"
            "    old_api = lambda value: value - 1\n"
            "    return old_api(2)\n"
        ),
        "py/use.py": "from api import old_api\nresult = old_api(1)\n",
    }
    for relative, source in sources.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source)
    configuration = {
        "project": "symbol-occurrence-projection",
        "layers": [
            {
                "globs": ["c/**"],
                "import_roots": ["c"],
                "language": "c",
                "name": "c",
            },
            {
                "globs": ["js/**"],
                "language": "javascript",
                "name": "javascript",
            },
            {
                "globs": ["py/**"],
                "import_roots": ["py"],
                "language": "python",
                "name": "python",
            },
        ],
    }
    project = Project.from_repository(
        root,
        config=json.dumps(configuration, separators=(",", ":")).encode(),
        jobs=1,
        map_cache=False,
    )
    map_json = project.map_json()

    def evaluate(
        name: str,
        path: str,
        *,
        classification: str = "incomplete",
        unknown: int = 1,
    ) -> dict[str, object]:
        definition = {
            "id": "rename-sites",
            "names": [name],
            "paths": [path],
            "select": "symbol_occurrences",
        }
        first = json.loads(evaluate_projection_json(map_json, definition))
        second = json.loads(evaluate_projection_json(map_json, definition))
        assert (
            first["projection_result_sha256"]
            == second["projection_result_sha256"]
        )
        assert first["completeness"]["classification"] == classification
        assert first["completeness"]["exhaustive"] is (
            classification == "complete"
        )
        assert first["completeness"]["counts"]["unknown"] == unknown
        for item in first["fact"]["items"]:
            attributes = item["attributes"]
            start = attributes["start_byte"]
            end = attributes["end_byte"]
            expected = name.rsplit(".", 1)[-1]
            assert sources[attributes["path"]].encode()[start:end] == expected.encode()
            assert all(
                fact_id.startswith("f:")
                for fact_id in attributes.get("fact_ids", [])
            )
        return first

    c_result = evaluate("old_api", "c/api.h")
    assert {
        (row["attributes"]["path"], row["attributes"]["role"], row["state"])
        for row in c_result["fact"]["items"]
    } == {
        ("c/api.c", "declaration", "current"),
        ("c/api.h", "declaration", "current"),
        ("c/use.c", "reference", "unknown"),
    }
    python_result = evaluate(
        "old_api", "py/api.py", classification="complete", unknown=0
    )
    assert {
        (row["attributes"]["path"], row["attributes"]["role"], row["state"])
        for row in python_result["fact"]["items"]
    } == {
        ("py/alias.py", "import", "current"),
        ("py/api.py", "declaration", "current"),
        ("py/shadow.py", "import", "current"),
        ("py/use.py", "import", "current"),
        ("py/use.py", "reference", "current"),
    }
    nested_result = evaluate("outer.old_api", "py/nested.py")
    assert {
        (row["attributes"]["path"], row["attributes"]["role"], row["state"])
        for row in nested_result["fact"]["items"]
    } == {
        ("py/nested.py", "declaration", "current"),
        ("py/nested.py", "reference", "unknown"),
    }
    javascript_result = evaluate("oldApi", "js/api.js")
    assert {
        (row["attributes"]["path"], row["attributes"]["role"], row["state"])
        for row in javascript_result["fact"]["items"]
    } == {
        ("js/alias.js", "export", "current"),
        ("js/alias.js", "import", "current"),
        ("js/api.js", "declaration", "current"),
        ("js/use.js", "import", "current"),
        ("js/use.js", "reference", "unknown"),
    }
    shutil.rmtree(root)


def main() -> None:
    _assert_project_configuration_conformance()
    _assert_import_resolution_soundness()
    _assert_partial_configuration_overlay()
    _assert_symbol_occurrence_projection()
    project_model = json.loads((ROOT / "archbird.json").read_text())
    project_model.pop("projections", None)
    project_model.pop("queries", None)
    project_model.pop("constraints", None)
    modern = {
        **project_model,
        "projections": {
            "core-engine": {
                "include": ["src/base/engine.c"],
                "select": "mapped_paths",
            },
            "public-api": {
                "paths": ["include/**"],
                "select": "symbols",
            }
        },
        "queries": {
            "ad-hoc": {
                "projection": "core-engine",
            },
            "api-impact": {
                "direction": "upstream",
                "projection": ["public-api", "core-engine"],
            },
            "inferred-impact": {
                "artifacts": ["browser-bundle"],
                "components": ["base"],
                "depth": 1,
                "direction": "both",
                "focus": ["test/test_project.c"],
                "packages": ["npm"],
                "paths": ["src/base/**"],
                "symbols": ["archbird_engine_create"],
            },
        },
        "constraints": {
            "DISJOINT": {
                "actual": {"literal": ["left"]},
                "assert": "disjoint",
                "expected": {"literal": ["right"]},
                "owner": "architecture",
                "rationale": "Independent literal domains remain disjoint.",
            },
            "REQUIRED": {
                "actual": {"literal": ["kept"]},
                "assert": "required_subset",
                "expected": {"literal": ["kept"]},
                "owner": "architecture",
                "rationale": "Required project fact remains present.",
            },
            "REQUIRED-EDGE": {
                "kind": "required_file_edge",
                "edge_kind": "declaration",
                "source": "include/archbird/archbird.h",
                "target": "src/base/engine.c",
                "name": "archbird_engine_create",
                "owner": "architecture",
                "rationale": "The public engine declaration remains implemented.",
            },
            "REQUIRED-ENTRYPOINT": {
                "kind": "required_package_entrypoint",
                "package": "npm",
                "route": "main",
                "target": "src/index.js",
                "owner": "architecture",
                "rationale": "The npm package retains its default entrypoint.",
            },
            "REQUIRED-MAPPED-PATH": {
                "kind": "required_paths",
                "paths": ["include/archbird/archbird.h"],
                "owner": "architecture",
                "rationale": "The public C header remains in configured Map scope.",
            },
            "REQUIRED-TEST-ROUTE": {
                "kind": "required_test_route",
                "group": "python",
                "target": "py/archbird/native.py",
                "owner": "architecture",
                "rationale": "Python native bindings retain a static test route.",
            },
        },
    }
    compiled = json.loads(
        compile_project_configuration(
            json.dumps(modern, sort_keys=True, separators=(",", ":")).encode()
        )
    )
    assert compiled["artifact"] == "project-configuration-plan"
    assert compiled["schema_version"] == 1
    assert compiled["map_overlay"]["project"] == "archbird"
    assert compiled["map_overlay"]["layers"] == modern["layers"]
    assert "queries" not in compiled["map_overlay"]
    assert "constraints" not in compiled["map_overlay"]

    modern_map = _map(modern)

    first_projection = json.loads(
        evaluate_projection_json(
            modern_map,
            {"id": "first", "paths": ["include/**"], "select": "symbols"},
        )
    )
    second_projection = json.loads(
        evaluate_projection_json(
            modern_map,
            {"id": "second", "paths": ["include/**"], "select": "symbols"},
        )
    )
    assert first_projection["projection_definition_sha256"] == second_projection[
        "projection_definition_sha256"
    ]
    assert first_projection["projection_result_sha256"] == second_projection[
        "projection_result_sha256"
    ]
    reordered_projection = json.loads(
        _native.projection_evaluate(
            modern_map,
            b'{"select":"symbols","paths":["include/**"],"id":"reordered"}',
        )
    )
    assert "id" not in reordered_projection["definition"]
    assert reordered_projection["projection_definition_sha256"] == first_projection[
        "projection_definition_sha256"
    ]
    assert reordered_projection["projection_result_sha256"] == first_projection[
        "projection_result_sha256"
    ]
    exact_symbol_projection = json.loads(
        evaluate_projection_json(
            modern_map,
            {
                "id": "exact-symbol",
                "names": ["archbird_engine_create"],
                "select": "symbols",
            },
        )
    )
    assert [
        row["key"] for row in exact_symbol_projection["fact"]["items"]
    ] == ["archbird_engine_create"]
    assert len(exact_symbol_projection["fact"]["items"][0]["evidence"]) >= 2

    edge_projection = json.loads(
        evaluate_projection_json(
            modern_map,
            {"id": "file-edge-names", "select": "file_edges"},
        )
    )
    edge_items = edge_projection["fact"]["items"]
    assert edge_items
    edge = edge_items[0]["attributes"]
    source_edge = next(
        row
        for row in json.loads(modern_map)["edges"]
        if row["source"] == edge["source"]
        and row["kind"] == edge["kind"]
        and row["target"] == edge["target"]
    )
    assert edge["names"] == source_edge["names"]

    inventory_map = json.loads(_fixture_map(ROOT / "test/fixtures/map_packages"))
    report_map = (ROOT / "test/fixtures/report_map.json").read_bytes()
    graph_definition = {
        "id": "architecture-graph",
        "select": "graph",
        "group_by": "directory",
        "level": "file",
        "relations": [
            "builds",
            "bridges",
            "declarations",
            "imports",
            "packages",
            "tests",
        ],
        "overlays": ["diagnostics", "evidence-quality"],
    }
    graph = json.loads(evaluate_projection_json(report_map, graph_definition))
    assert graph["fact"]["shape"] == "graph"
    assert graph["completeness"]["classification"] == "complete"
    coverage = next(
        row
        for row in graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "coverage"
    )
    assert coverage["state"] == "unknown"
    assert coverage["attributes"]["completeness_scope"] == "contextual"
    assert "discovery coverage" in coverage["message"]
    graph_records = {
        kind: [
            row
            for row in graph["fact"]["items"]
            if row["attributes"]["record_kind"] == kind
        ]
        for kind in (
            "diagnostic",
            "group",
            "group_relation",
            "ledger",
            "membership",
            "node",
            "relation",
        )
    }
    assert all(graph_records.values())
    graph_node_ids = {row["attributes"]["id"] for row in graph_records["node"]}
    assert "file:src/core.c" in graph_node_ids
    assert "package:npm" in graph_node_ids
    assert "build:build" in graph_node_ids
    assert {
        row["attributes"]["family"] for row in graph_records["relation"]
    } >= {"bridges", "builds", "declarations", "imports", "packages", "tests"}
    assert all(
        row["attributes"]["source"] in graph_node_ids
        and row["attributes"]["target"] in graph_node_ids
        for row in graph_records["relation"]
    )
    graph_group_ids = {
        row["attributes"]["id"] for row in graph_records["group"]
    }
    assert {"inventory:build", "inventory:external", "inventory:package"} <= (
        graph_group_ids
    )
    assert all(
        row["attributes"]["source"] in graph_group_ids
        and row["attributes"]["target"] in graph_group_ids
        and row["attributes"]["canonical_relation_keys"]
        and row["attributes"]["relation_kinds"]
        and row["attributes"]["relation_count"] >= 1
        for row in graph_records["group_relation"]
    )
    canonical_relation_keys = {
        key
        for row in graph_records["group_relation"]
        for key in row["attributes"]["canonical_relation_keys"]
    }
    assert canonical_relation_keys
    assert canonical_relation_keys <= {
        row["key"] for row in graph_records["relation"]
    }
    assert sum(
        row["attributes"].get("uses_count", 0)
        for row in graph_records["node"]
    ) == len(graph_records["relation"])
    assert sum(
        row["attributes"].get("used_by_count", 0)
        for row in graph_records["node"]
    ) == len(graph_records["relation"])
    assert sum(
        row["attributes"].get("relation_witness_count", 0)
        for row in graph_records["node"]
    ) == 2 * sum(
        row["attributes"]["witness_count"]
        for row in graph_records["relation"]
    )
    assert all(
        row["attributes"]["evidence_class"] in {"direct", "stale", "unknown"}
        for row in graph_records["node"] + graph_records["relation"]
    )
    external_build_map = json.loads(report_map)
    external_build_map["builds"][0]["paths"].append("/")
    external_build_graph = json.loads(
        evaluate_projection_json(
            json.dumps(external_build_map, separators=(",", ":")).encode(),
            graph_definition,
        )
    )
    external_build_nodes = {
        row["attributes"]["id"]
        for row in external_build_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "node"
    }
    assert "external:/" in external_build_nodes
    assert any(
        row["attributes"].get("family") == "builds"
        and row["attributes"].get("source") == "build:build"
        and row["attributes"].get("target") == "external:/"
        for row in external_build_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "relation"
    )
    file_level_test_map = json.loads(report_map)
    file_level_test_map["tests"][0]["cases"] = []
    file_level_test_graph = json.loads(
        evaluate_projection_json(
            json.dumps(file_level_test_map, separators=(",", ":")).encode(),
            {
                "id": "file-level-tests",
                "select": "graph",
                "group_by": "directory",
                "level": "file",
                "relations": ["tests"],
            },
        )
    )
    assert any(
        row["attributes"].get("family") == "tests"
        and row["attributes"].get("source") == "external:test/test_api.js"
        and row["attributes"].get("target") == "file:js/runtime.js"
        for row in file_level_test_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "relation"
    )
    reordered_graph = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "relations": list(reversed(graph_definition["relations"])),
                "level": "file",
                "overlays": list(reversed(graph_definition["overlays"])),
                "group_by": "directory",
                "select": "graph",
                "id": "same-architecture-graph",
            },
        )
    )
    assert graph["projection_definition_sha256"] == reordered_graph[
        "projection_definition_sha256"
    ]
    assert graph["projection_result_sha256"] == reordered_graph[
        "projection_result_sha256"
    ]
    incomplete_graph = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "incomplete-call-graph",
                "select": "graph",
                "group_by": "directory",
                "level": "file",
                "relations": ["calls"],
            },
        )
    )
    assert incomplete_graph["completeness"]["classification"] == "incomplete"
    assert any(
        row["attributes"]["record_kind"] == "node"
        for row in incomplete_graph["fact"]["items"]
    )
    assert [
        (
            row["attributes"]["family"],
            row["attributes"]["unknown"],
            row["state"],
        )
        for row in incomplete_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "ledger"
    ] == [("calls", 1, "unknown")]
    for view_index, view in enumerate(
        ("overview", "architecture", "tests", "evidence")
    ):
        report = render_map_markdown(report_map, view=view, detail="compact")
        assert report.startswith(b"# sample architecture evidence\n")
        assert b"## Graph completeness\n" in report
        tail = [line for line in report.splitlines() if line][-2:]
        assert tail[0].startswith(b"Result: ")
        assert tail[1].startswith(b"Evidence: graph=")
        assert b"projection=`" in tail[1]
        assert report == _native.map_markdown_view(
            report_map, view_index, 0, max_chars=0
        )
    budgeted_report = render_map_markdown(
        report_map, view="overview", detail="standard", max_chars=1_800
    )
    assert len(budgeted_report.decode("utf-8")) <= 1_800
    assert b"presentation-omitted=" in budgeted_report
    try:
        render_map_markdown(
            report_map, view="overview", detail="full", max_chars=1_800
        )
    except RuntimeError as error:
        assert "projection.max_chars cannot be combined with full detail" in str(
            error
        )
    else:
        raise AssertionError("full graph report accepted a presentation budget")
    tests_report = render_map_markdown(
        report_map, view="tests", detail="standard"
    )
    assert b"## Test route landmarks\n" in tests_report
    assert b"`py/sample/api.py`" not in tests_report
    evidence_report = render_map_markdown(
        report_map, view="evidence", detail="standard"
    )
    assert b"## Evidence accounting\n" in evidence_report
    assert b"relation-witnesses=0" not in evidence_report
    component_graph = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "component-graph",
                "select": "graph",
                "level": "component",
                "relations": ["declarations", "imports"],
            },
        )
    )
    component_nodes = {
        row["attributes"]["id"]: row
        for row in component_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "node"
    }
    assert "component:core" in component_nodes
    assert any(
        node["attributes"]["entity_kind"] == "unassigned"
        for node in component_nodes.values()
    )
    grouped_component_graph = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "component-file-graph",
                "select": "graph",
                "group_by": "component",
                "level": "file",
                "relations": ["imports"],
            },
        )
    )
    core_group = next(
        row
        for row in grouped_component_graph["fact"]["items"]
        if row["attributes"].get("record_kind") == "group"
        and row["attributes"].get("id") == "component:core"
    )
    assert core_group["attributes"]["description"] == ""
    assert core_group["attributes"]["files"] == ["src/core.c", "src/core.h"]
    assert core_group["attributes"]["symbol_count"] == 8
    assert core_group["attributes"]["member_count"] == 2
    no_component_inventory = json.loads(report_map)
    del no_component_inventory["components"]
    unavailable_component_graph = json.loads(
        evaluate_projection_json(
            json.dumps(
                no_component_inventory, sort_keys=True, separators=(",", ":")
            ).encode(),
            {
                "id": "missing-component-graph",
                "select": "graph",
                "group_by": "component",
                "level": "file",
                "relations": [],
            },
        )
    )
    assert unavailable_component_graph["completeness"]["classification"] == "unknown"
    assert unavailable_component_graph["fact"]["state"] == "unknown"
    assert [
        (row["key"], row["state"], row["message"])
        for row in unavailable_component_graph["fact"]["items"]
    ] == [
        (
            "projection",
            "unknown",
            "project map has no component/file inventory",
        )
    ]
    many_component_map = json.loads(report_map)
    many_component_map["components"] = [
        {
            "description": "",
            "files": ["src/core.h"] if index == 0 else ["src/core.c"],
            "name": f"overlap-{index:02d}",
            "outgoing": {},
            "symbol_count": 0,
        }
        for index in range(21)
    ]
    many_component_graph = json.loads(
        evaluate_projection_json(
            json.dumps(many_component_map, separators=(",", ":")).encode(),
            {
                "id": "many-component-graph",
                "select": "graph",
                "level": "component",
                "relations": ["declarations"],
            },
        )
    )
    assert len(
        [
            row
            for row in many_component_graph["fact"]["items"]
            if row["attributes"]["record_kind"] == "node"
            and row["attributes"]["entity_kind"] == "component"
        ]
    ) == 21
    assert len(
        {
            row["attributes"]["target"]
            for row in many_component_graph["fact"]["items"]
            if row["attributes"]["record_kind"] == "relation"
            and row["attributes"]["family"] == "declarations"
        }
    ) == 20
    symbol_graph = json.loads(
        evaluate_projection_json(
            modern_map,
            {
                "id": "symbol-graph",
                "select": "graph",
                "group_by": "layer",
                "level": "symbol",
                "relations": ["calls", "references"],
            },
        )
    )
    assert any(
        row["attributes"]["record_kind"] == "node"
        and row["attributes"]["entity_kind"] == "symbol"
        for row in symbol_graph["fact"]["items"]
    )
    assert {
        row["attributes"]["family"]
        for row in symbol_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "ledger"
    } == {"calls", "references"}
    for invalid_graph in (
        {
            "id": "missing-level",
            "select": "graph",
        },
        {
            "id": "grouped-components",
            "select": "graph",
            "level": "component",
            "group_by": "directory",
        },
        {
            "id": "duplicate-relations",
            "select": "graph",
            "level": "file",
            "relations": ["imports", "imports"],
        },
        {
            "id": "invalid-symbol-relation",
            "select": "graph",
            "level": "symbol",
            "relations": ["imports"],
        },
        {
            "id": "verification-is-not-map-derived",
            "select": "graph",
            "level": "file",
            "overlays": ["verification"],
        },
    ):
        try:
            evaluate_projection_json(report_map, invalid_graph)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"invalid graph projection accepted: {invalid_graph}")

    malformed = json.loads(json.dumps(inventory_map))
    malformed["files"][0]["symbols"][0].pop("kind")
    _assert_invalid_projection(
        malformed, {"id": "invalid-symbol", "select": "symbols"}
    )
    malformed = json.loads(json.dumps(inventory_map))
    malformed["edges"][0].pop("kind")
    _assert_invalid_projection(
        malformed, {"id": "invalid-edge", "select": "file_edges"}
    )
    malformed = json.loads(json.dumps(inventory_map))
    malformed["packages"][0]["entrypoints"]["main"] = 1
    _assert_invalid_projection(
        malformed,
        {"id": "invalid-package", "select": "package_entrypoints"},
    )
    malformed = json.loads(json.dumps(inventory_map))
    malformed["artifacts"][0]["inputs"][0].pop("path")
    _assert_invalid_projection(
        malformed, {"id": "invalid-artifact", "select": "artifact_routes"}
    )
    malformed = json.loads(json.dumps(inventory_map))
    malformed["tests"][0]["cases"][0].pop("selector")
    _assert_invalid_projection(
        malformed, {"id": "invalid-test", "select": "test_routes"}
    )

    component_map = json.loads(modern_map)
    populated_component = next(
        row for row in component_map["components"] if row["files"]
    )
    component_map["components"].append(
        {"files": populated_component["files"][:3], "name": "overlap-copy"}
    )
    memberships: dict[str, set[str]] = {}
    for component in component_map["components"]:
        for path in component["files"]:
            memberships.setdefault(path, set()).add(component["name"])
    expected_component_edges = {
        (source_component, edge["kind"], target_component)
        for edge in component_map["edges"]
        for source_component in memberships.get(edge["source"], ())
        for target_component in memberships.get(edge["target"], ())
        if source_component != target_component
    }
    component_projection = json.loads(
        evaluate_projection_json(
            json.dumps(component_map, separators=(",", ":")).encode(),
            {"id": "component-edges", "select": "component_edges"},
        )
    )
    assert {
        (
            row["attributes"]["source"],
            row["attributes"]["kind"],
            row["attributes"]["target"],
        )
        for row in component_projection["fact"]["items"]
        if row["state"] == "current"
    } == expected_component_edges

    config_json = json.dumps(
        modern, sort_keys=True, separators=(",", ":")
    ).encode()
    query_plan = compile_named_query(config_json, "api-impact")
    assert query_plan["id"] == "api-impact"
    assert query_plan["kind"] == "configured"
    assert len(query_plan["projections"]) == 2
    assert all(
        "projection_result_sha256" not in row
        for row in query_plan["projections"]
    )
    public_query_plan = json.loads(
        compile_query_plan_json(config_json, "api-impact")
    )
    assert public_query_plan == {
        "artifact": "query-plan",
        "plan": query_plan,
        "schema_version": 3,
    }
    named_query = json.loads(query_map_json(modern_map, plan=query_plan))
    assert named_query["query"]["plan"] == query_plan
    projection_results = named_query["query"]["projection_results"]
    assert len(projection_results) == 2
    assert [
        row["projection_definition_sha256"]
        for row in query_plan["projections"]
    ] == [
        row["projection_definition_sha256"] for row in projection_results
    ]
    assert all("projection_result_sha256" in row for row in projection_results)
    named_report = query_map_markdown(modern_map, plan=query_plan).decode()
    assert "Named query: `api-impact`" in named_report
    assert "Focus: ``" not in named_report
    if "Continue with:" in named_report:
        assert "Continue with: `archbird query api-impact " in named_report
    named_tail = [line for line in named_report.splitlines() if line][-3:]
    assert any(line.startswith("Emitted: files=") for line in named_tail)
    assert any(line.startswith("Not emitted: ") for line in named_tail)

    sentinel_named_plan = compile_named_query(config_json, "ad-hoc")
    assert sentinel_named_plan["id"] == "ad-hoc"
    assert sentinel_named_plan["kind"] == "configured"
    sentinel_named_query = json.loads(
        query_map_json(modern_map, plan=sentinel_named_plan)
    )
    assert sentinel_named_query["query"]["plan"] == sentinel_named_plan
    sentinel_named_report = query_map_markdown(
        modern_map, plan=sentinel_named_plan
    ).decode()
    assert "Named query: `ad-hoc`" in sentinel_named_report
    assert "Focus: ``" not in sentinel_named_report

    inferred_plan = compile_named_query(config_json, "inferred-impact")
    assert len(inferred_plan["projections"]) == 14
    inferred_query = json.loads(query_map_json(modern_map, plan=inferred_plan))
    assert len(inferred_query["query"]["projection_results"]) == 14
    inferred_report = query_map_markdown(modern_map, plan=inferred_plan).decode()
    assert "Named query: `inferred-impact`; focus: `" in inferred_report
    assert "test/test_project.c" in inferred_report
    direct_query = json.loads(
        query_map_json(
            modern_map,
            artifacts=["browser-bundle"],
            components=["base"],
            depth=1,
            direction="both",
            focus=["test/test_project.c"],
            packages=["npm"],
            paths=["src/base/**"],
            search_limit=8,
            symbols=["archbird_engine_create"],
            test_depth=8,
        )
    )
    for field in (
        "artifacts",
        "components",
        "edges",
        "files",
        "packages",
        "symbol_matches",
        "tests",
    ):
        assert inferred_query.get(field, []) == direct_query.get(field, []), field

    ad_hoc_plan = compile_ad_hoc_query({"paths": ["src/base/**"]})
    assert ad_hoc_plan["id"] == "ad-hoc"
    assert ad_hoc_plan["kind"] == "ad_hoc"
    assert ad_hoc_plan["project_configuration_sha256"] is None
    assert len(ad_hoc_plan["projections"]) == 1
    ad_hoc_query = json.loads(query_map_json(modern_map, plan=ad_hoc_plan))
    assert len(ad_hoc_query["query"]["projection_results"]) == 1
    direct_path_query = json.loads(
        query_map_json(modern_map, paths=["src/base/**"])
    )
    assert ad_hoc_query["files"] == direct_path_query["files"]

    same_line_config = {
        "project": "same-line-c-query",
        "layers": [
            {
                "globs": ["src/**/*.c"],
                "language": "c",
                "name": "c",
            }
        ],
    }
    same_line_project = Project(
        "same-line-c-query",
        (
            Source(
                "src/api.c",
                b"int archbird_pair(void); "
                b"int archbird_pair(void) { return 0; }\n",
                language="c",
                layer="c",
            ),
        ),
    )
    same_line_project.set_config(
        json.dumps(same_line_config, separators=(",", ":")).encode()
    )
    same_line_project.scan(jobs=1, map_cache=False)
    same_line_query = json.loads(
        query_map_json(
            same_line_project.map_json(),
            search=["archbird pair"],
            search_limit=8,
            depth=0,
            test_depth=0,
        )
    )
    same_line_hits = same_line_query["query"]["retrieval"]["hits"]
    assert {
        (row["name"], row["symbol_kind"], row["line"])
        for row in same_line_hits
        if row["kind"] == "symbol"
    } == {
        ("archbird_pair", "declaration", 1),
        ("archbird_pair", "function", 1),
    }
    assert same_line_query["query"]["projection_results"][0]["completeness"][
        "classification"
    ] == "complete"

    overload_config = {
        "project": "same-line-cpp-overloads",
        "layers": [
            {
                "globs": ["src/**/*.cpp"],
                "language": "cpp",
                "name": "cpp",
            }
        ],
    }
    overload_project = Project(
        "same-line-cpp-overloads",
        (
            Source(
                "src/api.cpp",
                b"int archbird_overload(int); "
                b"double archbird_overload(double);\n",
                language="cpp",
                layer="cpp",
            ),
        ),
    )
    overload_project.set_config(
        json.dumps(overload_config, separators=(",", ":")).encode()
    )
    overload_project.scan(jobs=1, map_cache=False)
    overload_query = json.loads(
        query_map_json(
            overload_project.map_json(),
            search=["archbird overload"],
            search_limit=8,
            depth=0,
            test_depth=0,
        )
    )
    overload_hits = [
        row
        for row in overload_query["query"]["retrieval"]["hits"]
        if row["kind"] == "symbol"
    ]
    assert [row["symbol_signature"] for row in overload_hits] == [
        "double archbird_overload(double)",
        "int archbird_overload(int)",
    ]
    assert overload_query["query"]["projection_results"][0]["completeness"][
        "classification"
    ] == "complete"

    normalized_symbol_config = {
        "project": "normalized-symbol-query",
        "layers": [
            {
                "globs": ["src/**/*.js"],
                "language": "javascript",
                "name": "javascript",
            }
        ],
        "projections": {
            "one": {
                "names": ["one"],
                "paths": ["src/same-line.js"],
                "select": "symbols",
                "strip_prefix": "api_",
            }
        },
        "queries": {"one": {"depth": 0, "projection": "one"}},
    }
    normalized_symbol_config_json = json.dumps(
        normalized_symbol_config, sort_keys=True, separators=(",", ":")
    ).encode()
    normalized_project = Project(
        "normalized-symbol-query",
        (
            Source(
                "src/same-line.js",
                b"function api_one() {} function api_two() {}\n",
                language="javascript",
                layer="javascript",
            ),
        ),
    )
    normalized_project.set_config(normalized_symbol_config_json)
    normalized_project.scan(jobs=1, map_cache=False)
    normalized_map = normalized_project.map_json()
    normalized_plan = compile_named_query(
        normalized_symbol_config_json, "one"
    )
    normalized_query = json.loads(
        query_map_json(normalized_map, plan=normalized_plan)
    )
    assert [
        row["name"]
        for row in normalized_query["files"][0]["symbols"]
    ] == ["api_one"]

    partial_config = {
        "project": "partial-constant-query",
        "layers": [
            {
                "globs": ["src/**/*.c"],
                "language": "c",
                "name": "c",
            }
        ],
        "projections": {
            "normalized": {
                "container": "Ops",
                "select": "constant_values",
                "strip_prefix": "API_",
            }
        },
        "queries": {"valid": {"depth": 0, "projection": "normalized"}},
        "constraints": {
            "INCOMPLETE-SHAPE": {
                "actual": {"projection": "normalized"},
                "assert": "acyclic",
                "owner": "test",
                "rationale": "Completeness is checked before predicate shape.",
            },
            "NORMALIZED-CONSTANTS": {
                "actual": {"projection": "normalized"},
                "assert": "values_equal",
                "expected": {"literal": {"TWO": 3}},
                "owner": "test",
                "rationale": "Incomplete normalized identities cannot pass.",
            }
        },
    }
    partial_config_json = json.dumps(
        partial_config, sort_keys=True, separators=(",", ":")
    ).encode()
    partial_project = Project(
        "partial-constant-query",
        (
            Source(
                "src/collision.c",
                b"enum Ops { API_ONE = 1, ONE = 2, API_TWO = 3 };\n",
                language="c",
                layer="c",
            ),
        ),
    )
    partial_project.set_config(partial_config_json)
    partial_project.scan(jobs=1, map_cache=False)
    partial_map = partial_project.map_json()
    partial_plan = compile_named_query(partial_config_json, "valid")
    partial_query = json.loads(query_map_json(partial_map, plan=partial_plan))
    partial_result = partial_query["query"]["projection_results"][0]
    assert partial_result["completeness"]["classification"] == "incomplete"
    assert partial_result["completeness"]["counts"]["unknown"] == 2
    assert [row["path"] for row in partial_query["files"]] == [
        "src/collision.c"
    ]
    partial_verification = json.loads(
        evaluate_constraints_json(partial_config_json, partial_map)
    )
    assert {
        row["id"]: row["status"] for row in partial_verification["constraints"]
    } == {
        "INCOMPLETE-SHAPE": "unknown",
        "NORMALIZED-CONSTANTS": "unknown",
    }

    invalid_named_plan = dict(query_plan)
    invalid_named_plan["project_configuration_sha256"] = None
    try:
        query_map_json(
            modern_map,
            plan=invalid_named_plan,
        )
    except RuntimeError as error:
        assert "identity does not match its kind" in str(error)
    else:
        raise AssertionError("named Query plan accepted missing configuration identity")

    missing_kind_plan = dict(query_plan)
    del missing_kind_plan["kind"]
    try:
        query_map_json(modern_map, plan=missing_kind_plan)
    except RuntimeError as error:
        assert "identities are invalid" in str(error)
    else:
        raise AssertionError("Query plan accepted a missing kind discriminator")

    invalid_ad_hoc_plan = dict(ad_hoc_plan)
    invalid_ad_hoc_plan["project_configuration_sha256"] = compiled[
        "project_configuration_sha256"
    ]
    try:
        query_map_json(
            modern_map,
            plan=invalid_ad_hoc_plan,
        )
    except RuntimeError as error:
        assert "identity does not match its kind" in str(error)
    else:
        raise AssertionError("ad-hoc Query plan accepted configuration identity")

    subject_root = ROOT / "test/fixtures/verification/subject"
    reference_root = ROOT / "test/fixtures/verification/reference"
    subject_map = _fixture_map(subject_root)
    reference_map = _fixture_map(reference_root)
    subject_document = json.loads(subject_map)
    reference_document = json.loads(reference_map)
    assert subject_document["schema_version"] == 11
    assert reference_document["schema_version"] == 11
    assert {row["domain"] for row in subject_document["facts"]} == {
        "constant-values",
        "export-origins",
        "exports",
        "macro-invocations",
    }
    assert {row["domain"] for row in reference_document["facts"]} == {
        "constant-memberships",
        "constant-values",
        "export-origins",
        "exports",
        "imported-names",
        "reexport-candidates",
    }
    malformed = json.loads(json.dumps(subject_document))
    malformed["facts"][0].pop("domain")
    _assert_invalid_projection(
        malformed,
        {
            "container": "PortOps",
            "id": "invalid-fact",
            "select": "constant_values",
        },
    )

    source_projections = {
        "reference-ops": {
            "container": "Ops",
            "select": "constant_values",
        },
        "reference-required": {
            "container": "GroupOp.Required",
            "select": "constant_memberships",
        },
        "subject-ops": {
            "container": "PortOps",
            "exclude": ["COUNT", "EXTRA"],
            "select": "constant_values",
            "strip_prefix": "PORT_",
        },
        "subject-required": {
            "call": "OPSET",
            "select": "macro_members",
            "selector": "PORT_REQUIRED",
            "strip_prefix": "PORT_",
        },
        "subject-values": {
            "container": "op_values",
            "exclude": ["EXTRA"],
            "select": "constant_values",
            "strip_prefix": "PORT_",
        },
    }
    projection_expectations = {
        "reference-ops": {"ADD": 1, "MUL": 2, "WAIT": 3},
        "reference-required": {"ADD": None, "WAIT": None},
        "subject-ops": {"ADD": 0, "TIMES": 1},
        "subject-required": {"ADD": None, "EXTRA": None},
        "subject-values": {"ADD": 1, "TIMES": 9},
    }
    for projection_id, definition in source_projections.items():
        selected_map = (
            reference_map
            if projection_id.startswith("reference-")
            else subject_map
        )
        projection = json.loads(
            evaluate_projection_json(
                selected_map, {"id": projection_id, **definition}
            )
        )
        assert projection["completeness"]["classification"] == "complete"
        assert projection["completeness"]["exhaustive"] is True
        assert {
            row["key"]: row["value"] for row in projection["fact"]["items"]
        } == projection_expectations[projection_id]

    source_config = json.loads((subject_root / "archbird.json").read_text())
    source_config["projections"] = source_projections
    source_config["constraints"] = {
        "PORT-OPS": {
            "actual": {"projection": "subject-ops"},
            "assert": "mapped_values_equal",
            "expected": {
                "map": "reference",
                "projection": "reference-ops",
            },
            "mapping": {"TIMES": "MUL"},
            "owner": "compiler-core",
            "rationale": "Every port operation retains its reference value.",
        },
        "SUBJECT-VALUES": {
            "actual": {"projection": "subject-values"},
            "assert": "values_equal",
            "expected": {"literal": {"ADD": 1, "TIMES": 9}},
            "owner": "compiler-core",
            "rationale": "The subject lookup table retains reviewed values.",
        },
    }
    source_result = json.loads(
        evaluate_constraints_json(
            json.dumps(source_config, separators=(",", ":")).encode(),
            subject_map,
            maps={"reference": {"map": reference_document}},
        )
    )
    assert {row["id"]: row["status"] for row in source_result["constraints"]} == {
        "PORT-OPS": "fail",
        "SUBJECT-VALUES": "pass",
    }

    changed_source_document = json.loads(subject_map)
    changed_fact = next(
        row
        for row in changed_source_document["facts"]
        if row["domain"] == "constant-values"
        and row.get("name") == "PORT_ADD"
        and row["attributes"].get("container") == "PortOps"
    )
    changed_fact["attributes"]["value"] = 99
    source_diff = json.loads(
        diff_maps_json(
            subject_map,
            json.dumps(changed_source_document, separators=(",", ":")).encode(),
        )
    )
    assert len(source_diff["sections"]["facts"]["changed"]) == 1
    assert source_diff["sections"]["facts"]["changed"][0].startswith(
        changed_fact["id"] + ": "
    )

    provider_root = ROOT / "test/fixtures/act/provider"
    provider_reference = _fixture_map(provider_root / "reference")
    provider_subject = _fixture_map(provider_root / "subject")
    provider_reference_document = json.loads(provider_reference)
    provider_subject_document = json.loads(provider_subject)
    make_input = next(
        row
        for row in provider_subject_document["inputs"]
        if row["path"] == "Makefile"
    )
    assert make_input["roles"] == ["build", "provider"]
    assert len(make_input["sha256"]) == 64
    assert not any(
        row["path"] == "Makefile" for row in provider_subject_document["files"]
    )
    provider_projection = json.loads(
        evaluate_projection_json(
            provider_subject,
            {"id": "ffi-surface", "name": "ffi", "select": "provider_surface"},
        )
    )
    assert provider_projection["completeness"]["classification"] == "complete"
    assert provider_projection["fact"]["items"][0]["state"] == "current"
    malformed = json.loads(json.dumps(provider_subject_document))
    malformed["surfaces"][0]["names"][0].pop("resolution")
    _assert_invalid_projection(
        malformed,
        {"id": "invalid-surface", "name": "ffi", "select": "provider_surface"},
    )
    try:
        evaluate_projection_json(
            provider_subject,
            {"id": "invalid", "select": "provider_surface", "surface": "ffi"},
        )
    except RuntimeError as error:
        assert "field unsupported by its select operator" in str(error)
    else:
        raise AssertionError("incomplete provider-surface projection was accepted")
    provider_config = (provider_root / "subject/archbird.json").read_bytes()
    provider_verification = evaluate_constraints_json(
        provider_config,
        provider_subject,
        maps={"reference": {"map": provider_reference_document}},
    )
    provider_result = json.loads(provider_verification)
    assert {row["id"]: row["status"] for row in provider_result["constraints"]} == {
        "PROVIDER-RENAME": "fail",
        "PROVIDER-TEST-ROUTES": "pass",
    }
    provider_finding = provider_result["constraints"][0]["findings"][0]
    assert provider_finding["evidence_state"] == "current"
    changed_provider_document = json.loads(provider_subject)
    changed_provider_input = next(
        row
        for row in changed_provider_document["inputs"]
        if row["path"] == "Makefile"
    )
    changed_provider_input["sha256"] = "0" * 64
    provider_diff = json.loads(
        diff_maps_json(
            provider_subject,
            json.dumps(changed_provider_document, separators=(",", ":")).encode(),
        )
    )
    assert provider_diff["sections"]["inputs"]["changed"] == [
        "Makefile: " + make_input["sha256"] + " -> " + "0" * 64
    ]

    changed = json.loads(json.dumps(modern))
    changed["queries"]["api-impact"]["depth"] = 5
    changed["constraints"]["DISJOINT"]["rationale"] = "Reviewed wording only."
    changed_compiled = json.loads(
        compile_project_configuration(
            json.dumps(changed, sort_keys=True, separators=(",", ":")).encode()
        )
    )
    assert changed_compiled["project_configuration_sha256"] != compiled[
        "project_configuration_sha256"
    ]
    assert changed_compiled["map_overlay_sha256"] == compiled["map_overlay_sha256"]
    changed_map = _map(changed)
    assert changed_map == modern_map

    metadata_only = json.loads(json.dumps(modern))
    metadata_only["constraints"]["DISJOINT"]["rationale"] = (
        "Reviewed wording only."
    )
    metadata_plan = compile_named_query(
        json.dumps(metadata_only, sort_keys=True, separators=(",", ":")).encode(),
        "api-impact",
    )
    assert metadata_plan["query_plan_sha256"] == query_plan["query_plan_sha256"]
    assert metadata_plan["project_configuration_sha256"] != query_plan[
        "project_configuration_sha256"
    ]
    assert metadata_plan["projections"] == query_plan["projections"]
    mismatched_model = json.loads(json.dumps(modern))
    mismatched_model["layers"][0]["globs"].append("not-present/**/*.c")
    mismatched_json = json.dumps(
        mismatched_model, sort_keys=True, separators=(",", ":")
    ).encode()
    mismatched_plan = compile_named_query(
        mismatched_json, "api-impact"
    )
    assert "map_config_sha256" not in mismatched_plan
    assert mismatched_plan["query_plan_sha256"] == query_plan["query_plan_sha256"]
    assert json.loads(query_map_json(modern_map, plan=mismatched_plan))["artifact"] == "query"
    assert json.loads(evaluate_constraints_json(mismatched_json, modern_map))[
        "artifact"
    ] == "verification"
    overridden_plan = compile_named_query(
        json.dumps(modern, sort_keys=True, separators=(",", ":")).encode(),
        "api-impact",
        overrides={"depth": 3},
    )
    assert overridden_plan["query_plan_sha256"] != query_plan["query_plan_sha256"]

    reordered = json.loads(json.dumps(modern))
    reordered["queries"]["api-impact"]["projection"] = [
        "core-engine",
        "public-api",
        {"paths": ["include/**"], "select": "symbols"},
    ]
    reordered_plan = compile_named_query(
        json.dumps(reordered, sort_keys=True, separators=(",", ":")).encode(),
        "api-impact",
    )
    assert reordered_plan["query_plan_sha256"] == query_plan["query_plan_sha256"]
    assert reordered_plan["projections"] == query_plan["projections"]

    config_json = json.dumps(
        modern, sort_keys=True, separators=(",", ":")
    ).encode()
    result_bytes = evaluate_constraints_json(config_json, modern_map)
    result = json.loads(result_bytes)
    assert result["artifact"] == "verification"
    assert result["schema_version"] == 2
    assert [row["id"] for row in result["constraints"]] == [
        "DISJOINT",
        "REQUIRED",
        "REQUIRED-EDGE",
        "REQUIRED-ENTRYPOINT",
        "REQUIRED-MAPPED-PATH",
        "REQUIRED-TEST-ROUTE",
    ]
    assert all(row["status"] == "pass" for row in result["constraints"])
    assert result["summary"]["constraints"]["pass"] == 6
    assert "checks" not in result["summary"]
    assert "contract" not in result
    assert "extractors" not in result
    assert len(result["verification_result_sha256"]) == 64
    assert result["policy"]["kind"] == "all"
    assert result["policy"]["configured_count"] == 6
    assert result["policy"]["evaluated_count"] == 6
    for identity in result["policy"]["constraints"]:
        assert len(identity["constraint_definition_sha256"]) == 64
        assert len(identity["constraint_plan_sha256"]) == 64
        assert len(identity["constraint_result_sha256"]) == 64

    published = json.loads(
        publish_okf_bundle(modern_map, verification_json=result_bytes)
    )
    paths = {row["path"] for row in published["files"]}
    assert "verification/policy.md" in paths
    assert "verification/project.md" in paths
    assert "verification/diagnostics.md" in paths
    assert any(path.startswith("verification/constraints/") for path in paths)
    assert any(path.startswith("verification/operands/") for path in paths)
    assert not any(path.startswith("verification/checks/") for path in paths)
    assert not any(path.startswith("verification/facts/") for path in paths)
    assert not any(path.startswith("verification/projects/") for path in paths)

    selected = json.loads(
        evaluate_constraints_json(
            config_json, modern_map, constraint_ids=("REQUIRED",)
        )
    )
    assert [row["id"] for row in selected["constraints"]] == ["REQUIRED"]
    assert selected["policy"]["kind"] == "selected"
    assert selected["policy"]["requested_ids"] == ["REQUIRED"]
    assert selected["policy"]["omitted_count"] == 5

    symbol_constraint_config = json.loads(json.dumps(modern))
    symbol_constraint_config["constraints"] = {
        "REQUIRED-SYMBOL": {
            "kind": "required_symbols",
            "owner": "architecture",
            "rationale": "The public engine constructor remains present.",
            "symbols": ["archbird_engine_create"],
        }
    }
    symbol_constraint = json.loads(
        evaluate_constraints_json(
            json.dumps(symbol_constraint_config, separators=(",", ":")).encode(),
            modern_map,
        )
    )
    derived_symbol_operands = [
        row
        for row in symbol_constraint["operands"]
        if row["provenance"] == "derived"
    ]
    assert symbol_constraint["constraints"][0]["status"] == "pass"
    assert len(derived_symbol_operands) == 1
    assert [
        row["key"] for row in derived_symbol_operands[0]["items"]
    ] == ["archbird_engine_create"]

    observation_config = json.loads(json.dumps(modern))
    observation_config["constraints"]["PYTHON-PARITY"] = {
        "actual": {"observation": "subject"},
        "assert": "observations_equal",
        "expected": {"observation": "reference"},
        "owner": "portability",
        "rationale": "The Python route retains reviewed reference behavior.",
        "reference_route": "reference",
        "required_routes": ["python"],
    }
    observation_config_json = json.dumps(
        observation_config, sort_keys=True, separators=(",", ":")
    ).encode()
    observations = {
        "reference": json.loads(
            (ROOT / "test/fixtures/verification/reference.observation.json").read_text()
        ),
        "subject": json.loads(
            (ROOT / "test/fixtures/verification/subject.observation.json").read_text()
        ),
    }
    parity = json.loads(
        evaluate_constraints_json(
            observation_config_json,
            modern_map,
            constraint_ids=("PYTHON-PARITY",),
            observations=observations,
        )
    )
    assert parity["constraints"][0]["status"] == "pass"
    assert parity["policy"]["kind"] == "selected"
    assert [row["id"] for row in parity["observations"]] == [
        "reference",
        "subject",
    ]
    assert all("observation" in row for row in parity["observations"])
    assert "attestations" not in parity
    assert evaluate_constraints_json(
        observation_config_json,
        modern_map,
        constraint_ids=("PYTHON-PARITY",),
        observations=observations,
        format="markdown",
    ).startswith(b"# Architecture constraints: archbird\n")

    browser_config = json.loads(json.dumps(observation_config))
    browser_config["constraints"]["PYTHON-PARITY"]["required_routes"] = [
        "browser"
    ]
    browser_verification = evaluate_constraints_json(
        json.dumps(
            browser_config, sort_keys=True, separators=(",", ":")
        ).encode(),
        modern_map,
        observations=observations,
    )
    browser_result = json.loads(browser_verification)
    assert browser_verification == json.dumps(
        browser_result, sort_keys=True, separators=(",", ":")
    ).encode()
    browser_constraint = next(
        row
        for row in browser_result["constraints"]
        if row["id"] == "PYTHON-PARITY"
    )
    assert browser_constraint["status"] == "fail"
    assert browser_result["summary"]["blocking"] is True
    try:
        evaluate_constraints_json(
            observation_config_json,
            modern_map,
            constraint_ids=("PYTHON-PARITY",),
        )
    except RuntimeError as error:
        assert "unsupplied observation" in str(error)
    else:
        raise AssertionError("missing observation input was accepted")

    mismatched = json.loads(json.dumps(observations))
    mismatched["subject"]["id"] = "wrong"
    try:
        evaluate_constraints_json(
            observation_config_json,
            modern_map,
            constraint_ids=("PYTHON-PARITY",),
            observations=mismatched,
        )
    except RuntimeError as error:
        assert "observation id does not match key" in str(error)
    else:
        raise AssertionError("mismatched observation identity was accepted")

    cross_map_config = json.loads(json.dumps(modern))
    reference_header = next(
        row
        for row in json.loads(modern_map)["inputs"]
        if row["path"] == "include/archbird/archbird.h"
    )
    cross_map_config["constraints"]["CROSS-MAP-API"] = {
        "actual": {"projection": "public-api"},
        "assert": "set_equal",
        "expected": {
            "projection": "public-api",
            "map": "reference",
            "source_lock": {
                "include/archbird/archbird.h": reference_header["sha256"]
            },
        },
        "owner": "architecture",
        "rationale": "The public API matches a supplied reference Map.",
    }
    cross_map_config_json = json.dumps(
        cross_map_config, sort_keys=True, separators=(",", ":")
    ).encode()
    map_inputs = {"reference": {"map": json.loads(modern_map)}}
    cross_map = json.loads(
        evaluate_constraints_json(
            cross_map_config_json,
            modern_map,
            constraint_ids=("CROSS-MAP-API",),
            maps=map_inputs,
        )
    )
    assert cross_map["constraints"][0]["status"] == "pass"
    assert [row["id"] for row in cross_map["evaluations"]] == [
        "current",
        "reference",
    ]
    assert len(cross_map["operands"]) == 2
    assert len(cross_map["operand_definitions"]) == 2
    assert cross_map["operands"][0]["name"] != cross_map["operands"][1]["name"]
    assert any(
        row.get("source_lock")
        == {"include/archbird/archbird.h": reference_header["sha256"]}
        for row in cross_map["operand_definitions"].values()
    )
    stale_cross_map_config = json.loads(json.dumps(cross_map_config))
    stale_cross_map_config["constraints"]["CROSS-MAP-API"]["expected"][
        "source_lock"
    ]["include/archbird/archbird.h"] = "0" * 64
    stale_cross_map = json.loads(
        evaluate_constraints_json(
            json.dumps(stale_cross_map_config, separators=(",", ":")).encode(),
            modern_map,
            maps=map_inputs,
            constraint_ids=("CROSS-MAP-API",),
        )
    )
    assert stale_cross_map["constraints"][0]["status"] == "unknown"
    assert stale_cross_map["summary"]["blocking"] is True
    stale_markdown = evaluate_constraints_json(
        json.dumps(stale_cross_map_config, separators=(",", ":")).encode(),
        modern_map,
        maps=map_inputs,
        constraint_ids=("CROSS-MAP-API",),
        format="markdown",
    )
    stale_tail = [line for line in stale_markdown.splitlines() if line][-2:]
    assert (
        stale_tail[0]
        == b"Result: blocking=yes; constraints pass=0 fail=0 unknown=1 "
        b"waived=0 not-applicable=0; findings=1."
    )
    assert stale_tail[1].startswith(b"Evidence: coverage-keys=")
    assert any(
        "source lock mismatch: include/archbird/archbird.h" in row["message"]
        for row in stale_cross_map["operands"]
    )
    try:
        evaluate_constraints_json(
            cross_map_config_json,
            modern_map,
            constraint_ids=("CROSS-MAP-API",),
        )
    except RuntimeError as error:
        assert 'unsupplied Map "reference"' in str(error)
        assert "--map-input reference=PATH" in str(error)
    else:
        raise AssertionError("missing cross-Map input was accepted")

    markdown = evaluate_constraints_json(
        config_json,
        modern_map,
        constraint_ids=("REQUIRED",),
        format="markdown",
    )
    native_markdown, native_blocking = (
        _native.constraints_report_with_blocking(
            config_json,
            modern_map,
            "markdown",
            request_json=b'{"ids":["REQUIRED"]}',
        )
    )
    assert native_markdown == markdown
    assert native_blocking is False
    assert markdown.startswith(b"# Architecture constraints: archbird\n")
    assert b"constraint policy" in markdown
    assert b"## Constraints\n" in markdown
    assert markdown.endswith(
        b"Result: blocking=no; constraints pass=1 fail=0 unknown=0 waived=0 "
        b"not-applicable=0; findings=0.\n"
        b"Evidence: coverage-keys=1; coverage-regressions=0; diagnostics "
        b"errors=0 warnings=0.\n"
    )
    assert b"suite" not in markdown
    assert b"## Checks\n" not in markdown
    sarif = json.loads(
        evaluate_constraints_json(
            config_json,
            modern_map,
            constraint_ids=("REQUIRED",),
            format="sarif",
        )
    )
    automation = sarif["runs"][0]["automationDetails"]
    assert automation["properties"]["archbirdPolicyKind"] == "constraints"
    assert len(automation["properties"]["constraintPolicySha256"]) == 64
    junit = evaluate_constraints_json(
        config_json,
        modern_map,
        constraint_ids=("REQUIRED",),
        format="junit",
    )
    assert b"archbird.constraint_policy_sha256" in junit
    assert b"archbird.suite_sha256" not in junit
    frozen = json.loads(
        freeze_constraints_json(
            config_json,
            modern_map,
            owner="architecture",
            rationale="Reviewed initial constraint baseline.",
        )
    )
    assert frozen["artifact"] == "constraint-baseline"
    assert frozen["schema_version"] == 1
    assert frozen["constraint_policy_sha256"] == compiled[
        "constraint_policy_sha256"
    ]
    assert frozen["active"] == []

    failing = json.loads(json.dumps(modern))
    failing["constraints"]["MISSING-LITERAL"] = {
        "actual": {"literal": []},
        "assert": "required_subset",
        "expected": {"literal": ["required"]},
        "owner": "architecture",
        "rationale": "A reviewed literal remains present.",
    }
    failing_json = json.dumps(
        failing, sort_keys=True, separators=(",", ":")
    ).encode()
    failing_verification = evaluate_constraints_json(failing_json, modern_map)
    failing_result = json.loads(failing_verification)
    missing = next(
        row for row in failing_result["constraints"] if row["id"] == "MISSING-LITERAL"
    )
    assert missing["status"] == "fail"
    failing_markdown = evaluate_constraints_json(
        failing_json, modern_map, format="markdown"
    )
    native_failing_markdown, native_failing_blocking = (
        _native.constraints_report_with_blocking(
            failing_json,
            modern_map,
            "markdown",
        )
    )
    assert native_failing_markdown == failing_markdown
    assert native_failing_blocking is True
    failing_tail = [line for line in failing_markdown.splitlines() if line][-2:]
    assert (
        failing_tail[0]
        == b"Result: blocking=yes; constraints pass=6 fail=1 unknown=0 "
        b"waived=0 not-applicable=0; findings=1."
    )
    assert failing_tail[1].startswith(b"Evidence: coverage-keys=")
    fingerprint = missing["findings"][0]["fingerprint"]
    waived = json.loads(json.dumps(failing))
    waived["constraints"]["MISSING-LITERAL"]["waivers"] = [
        {
            "comparison": "missing",
            "expires_on": "2026-12-31",
            "id": "KNOWN-MISSING-LITERAL",
            "key": "required",
            "owner": "architecture",
            "rationale": "Reviewed temporary absence while the API migrates.",
        }
    ]
    waived_json = json.dumps(waived, separators=(",", ":")).encode()
    waived_result = json.loads(
        evaluate_constraints_json(
            waived_json, modern_map, policy_date="2026-07-21"
        )
    )
    waived_missing = next(
        row
        for row in waived_result["constraints"]
        if row["id"] == "MISSING-LITERAL"
    )
    assert waived_missing["status"] == "waived"
    assert waived_missing["findings"][0]["disposition"] == "waived"
    assert waived_missing["findings"][0]["waiver"] == "KNOWN-MISSING-LITERAL"
    assert waived_result["summary"]["blocking"] is False
    assert waived_result["policy"]["policy_date"] == "2026-07-21"

    expired_result = json.loads(
        evaluate_constraints_json(
            waived_json, modern_map, policy_date="2027-01-01"
        )
    )
    expired_missing = next(
        row
        for row in expired_result["constraints"]
        if row["id"] == "MISSING-LITERAL"
    )
    assert expired_missing["status"] == "fail"
    assert expired_missing["findings"][0]["disposition"] == "open"
    assert expired_missing["findings"][0]["waiver_note"].startswith(
        "expired on 2026-12-31"
    )
    assert any(
        row["code"] == "waiver-inactive"
        for row in expired_result["diagnostics"]
    )

    bounded = json.loads(json.dumps(failing))
    bounded["constraints"]["MISSING-LITERAL"]["waivers"] = [
        {
            "fingerprint": fingerprint,
            "id": "STATE-BOUND-MISSING-LITERAL",
            "owner": "architecture",
            "rationale": "Reviewed only for the current repository input.",
            "until_inputs": {
                "current": json.loads(modern_map)["evidence"]["input_sha256"]
            },
        }
    ]
    bounded_result = json.loads(
        evaluate_constraints_json(
            json.dumps(bounded, separators=(",", ":")).encode(), modern_map
        )
    )
    bounded_missing = next(
        row
        for row in bounded_result["constraints"]
        if row["id"] == "MISSING-LITERAL"
    )
    assert bounded_missing["status"] == "waived"
    failing_baseline = json.loads(
        freeze_constraints_json(
            failing_json,
            modern_map,
            owner="architecture",
            rationale="Review the current violation before ratcheting it down.",
        )
    )
    assert failing_baseline["artifact"] == "constraint-baseline"
    assert failing_baseline["active"]
    baselined_result = json.loads(
        evaluate_constraints_json(
            failing_json,
            modern_map,
            baseline=failing_baseline,
        )
    )
    assert baselined_result["policy"]["kind"] == "all"
    assert baselined_result["policy"]["requested_ids"] == []
    baselined_missing = next(
        row
        for row in baselined_result["constraints"]
        if row["id"] == "MISSING-LITERAL"
    )
    assert baselined_missing["findings"][0]["baseline_state"] == "known"


if __name__ == "__main__":
    main()
