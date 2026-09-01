from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

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
    maximum_id = "a" * 256
    compile_project_configuration(
        json.dumps(
            {
                "project": "bounded-identities",
                "projections": {maximum_id: {"select": "mapped_paths"}},
            }
        ).encode()
    )
    oversized_id = "a" * 257
    try:
        compile_project_configuration(
            json.dumps(
                {
                    "project": "bounded-identities",
                    "projections": {
                        oversized_id: {"select": "mapped_paths"}
                    },
                }
            ).encode()
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("project configuration accepted a 257-byte stable ID")
    control_path = {
        "project": "bounded-paths",
        "ordered_containers": [
            {
                "language": "c",
                "names": ["rules"],
                "paths": ["src/\u0001rules.c"],
            }
        ],
    }
    try:
        compile_project_configuration(json.dumps(control_path).encode())
    except RuntimeError:
        pass
    else:
        raise AssertionError("project configuration accepted a control-byte path")
    matrix = next(
        entry
        for entry in corpus["cases"]
        if entry["id"] == "all-projection-operators"
    )
    expected_shapes = {
        "artifact_routes": "relation",
        "build_routes": "relation",
        "component_edges": "relation",
        "component_membership": "values",
        "components": "set",
        "constant_memberships": "set",
        "constant_values": "values",
        "file_edges": "relation",
        "file_metrics": "values",
        "graph": "graph",
        "inventory_paths": "set",
        "macro_members": "set",
        "mapped_paths": "set",
        "metrics": "values",
        "ordered_rows": "values",
        "package_entrypoints": "relation",
        "package_exports": "relation",
        "provider_surface": "values",
        "search_domain": "set",
        "symbol_entities": "set",
        "symbol_occurrences": "set",
        "symbol_relations": "relation",
        "symbols": "set",
        "test_routes": "relation",
        "test_selectors": "set",
    }
    projections = matrix["configuration"]["projections"]
    assert len(projections) == len(expected_shapes)
    assert {row["select"] for row in projections.values()} == set(expected_shapes)
    report_map = (ROOT / "test/fixtures/report_map.json").read_bytes()
    for operation in (
        lambda: evaluate_projection_json(
            report_map,
            {"id": oversized_id, "select": "mapped_paths"},
        ),
        lambda: compile_query_plan_json(b"{}", oversized_id),
    ):
        try:
            operation()
        except RuntimeError:
            pass
        else:
            raise AssertionError("public API accepted a 257-byte stable ID")
    for projection_id, projection in projections.items():
        result = json.loads(
            evaluate_projection_json(
                report_map, {"id": projection_id, **projection}
            )
        )
        assert result["definition"]["select"] == projection["select"]
        assert result["fact"]["shape"] == expected_shapes[projection["select"]]
    required_field_cases = {
        "constant_memberships": "constant projection requires a container",
        "constant_values": "constant projection requires a container",
        "file_metrics": "file_metrics requires metric bytes",
        "graph": "graph requires level component, file, or symbol",
        "macro_members": "macro_members requires call and selector",
        "metrics": "metrics requires a metric and level",
        "ordered_rows": "ordered_rows requires a container",
        "provider_surface": "provider_surface requires a name",
        "symbol_occurrences": (
            "symbol_occurrences requires exactly one non-empty symbol name"
        ),
    }
    for select, message in required_field_cases.items():
        configuration = {
            "project": "conformance",
            "layers": [
                {"name": "c", "language": "c", "globs": ["src/**/*.c"]}
            ],
            "projections": {"missing-required": {"select": select}},
        }
        try:
            compile_project_configuration(json.dumps(configuration).encode())
        except RuntimeError as error:
            assert str(error) == f"projection plan: {message} (status=8)"
        else:
            raise AssertionError(
                f"projection {select} accepted missing required fields"
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


def _assert_metrics_projection() -> None:
    invalid_metrics = [
        (
            {"metric": "source.extent_lines", "level": "file"},
            "source.extent_lines requires level symbol",
        ),
        (
            {
                "metric": "source.extent_lines",
                "level": "symbol",
                "relations": ["calls"],
            },
            "source.extent_lines does not accept graph relations",
        ),
        (
            {"metric": "graph.out_neighbors", "level": "file"},
            "graph.out_neighbors requires unique relations",
        ),
        (
            {
                "metric": "graph.out_neighbors",
                "level": "symbol",
                "relations": ["imports"],
            },
            "symbol graph metrics support calls and references",
        ),
        (
            {"metric": "graph.in_neighbors", "level": "file"},
            "graph.in_neighbors requires unique relations",
        ),
        (
            {
                "metric": "graph.internal_relations",
                "level": "file",
                "relations": ["imports"],
            },
            "graph.internal_relations requires group_by component or layer",
        ),
        (
            {
                "metric": "graph.inbound_relations",
                "level": "file",
                "group_by": "directory",
                "relations": ["imports"],
            },
            "graph.inbound_relations requires group_by component or layer",
        ),
        (
            {
                "metric": "graph.outbound_group_neighbors",
                "level": "file",
                "relations": ["imports"],
            },
            "graph.outbound_group_neighbors requires group_by component or layer",
        ),
        (
            {"metric": "tests.observed_cases", "level": "file"},
            "tests.observed_cases requires level map",
        ),
        (
            {"metric": "evidence.current_relations", "level": "file"},
            "evidence.current_relations requires unique relations",
        ),
        (
            {
                "metric": "graph.scc_size",
                "level": "symbol",
                "relations": ["imports"],
            },
            "symbol graph metrics support calls and references",
        ),
    ]
    for definition, message in invalid_metrics:
        configuration = {
            "project": "invalid-metric",
            "projections": {
                "invalid": {"select": "metrics", **definition},
            },
        }
        try:
            compile_project_configuration(
                json.dumps(configuration, separators=(",", ":")).encode()
            )
        except RuntimeError as error:
            assert message in str(error)
        else:
            raise AssertionError(f"invalid metric projection accepted: {definition}")

    fixture_map = _fixture_map(ROOT / "test/fixtures/map_base")
    decoded = json.loads(fixture_map)
    definitions = {
        (file["path"], symbol["name"]): symbol
        for file in decoded["files"]
        for symbol in file["symbols"]
    }
    assert definitions[("js/index.js", "add")]["extent_lines"] == 3
    assert definitions[("js/index.js", "twice")]["extent_lines"] == 1
    assert definitions[("py/pkg/api.py", "add")]["extent_lines"] == 2
    assert definitions[("py/pkg/api.py", "twice")]["extent_lines"] == 2
    summaries = {
        file["path"]: file["metrics"]["source.extent_lines"]
        for file in decoded["files"]
    }
    assert summaries["js/index.js"] == {
        "current": 2,
        "max": 3,
        "max_fact_id": definitions[("js/index.js", "add")]["fact_id"],
        "max_symbol": "add",
        "unknown": 0,
    }
    assert summaries["py/pkg/api.py"] == {
        "current": 2,
        "max": 2,
        "max_fact_id": definitions[("py/pkg/api.py", "add")]["fact_id"],
        "max_symbol": "add",
        "unknown": 0,
    }
    assert summaries["py/pkg/__init__.py"] == {
        "current": 0,
        "max": 0,
        "unknown": 0,
    }

    extent = json.loads(
        evaluate_projection_json(
            fixture_map,
            {
                "id": "source-extents",
                "select": "metrics",
                "metric": "source.extent_lines",
                "level": "symbol",
            },
        )
    )
    assert extent["fact"]["shape"] == "values"
    assert extent["completeness"]["classification"] == "complete"
    assert {item["value"] for item in extent["fact"]["items"]} == {1, 2, 3}
    assert all(
        item["attributes"]["definition"] == "source.extent-lines-v1"
        and item["attributes"]["unit"] == "line"
        for item in extent["fact"]["items"]
    )
    policy = {
        "project": "map-base",
        "projections": {
            "source-extents": {
                "select": "metrics",
                "metric": "source.extent_lines",
                "level": "symbol",
            }
        },
        "constraints": {
            "MAX-DECLARATION-EXTENT": {
                "assert": "numeric_bounds",
                "actual": {"projection": "source-extents"},
                "max": 2,
                "owner": "architecture",
                "rationale": "Exercise an explicit project-owned metric bound.",
            }
        },
    }
    bounded = json.loads(
        evaluate_constraints_json(
            json.dumps(policy, separators=(",", ":")).encode(), fixture_map
        )
    )
    assert bounded["constraints"][0]["status"] == "fail"
    incomplete_map = json.loads(fixture_map)
    definitions = [
        symbol
        for file in incomplete_map["files"]
        for symbol in file["symbols"]
    ]
    definitions[0].pop("extent_lines")
    incomplete_extent = json.loads(
        evaluate_constraints_json(
            json.dumps(policy, separators=(",", ":")).encode(),
            json.dumps(incomplete_map, separators=(",", ":")).encode(),
        )
    )
    assert incomplete_extent["constraints"][0]["status"] == "unknown"

    complexity_map = _fixture_map(ROOT / "test/fixtures/source_complexity")
    complexity_document = json.loads(complexity_map)
    unsupported_complexity = dict(complexity_document)
    unsupported_complexity["schema_version"] = 16
    try:
        query_map_markdown(
            json.dumps(unsupported_complexity, separators=(",", ":")).encode(),
            symbols=["jsFlow"],
        )
    except RuntimeError as error:
        assert "schema 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, or 15" in str(
            error
        )
    else:
        raise AssertionError("unsupported Map schema was accepted")
    complexity_symbols = {
        (file["path"], symbol["name"]): symbol
        for file in complexity_document["files"]
        for symbol in file["symbols"]
    }
    expected_complexity = {
        ("src/control.c", "c_flow"): (10, 11, 3, "source.control-flow.c-v1"),
        ("src/control.cpp", "cpp_flow"): (4, 5, 3, "source.control-flow.cpp-v1"),
        ("src/control.cpp", "cpp_outer"): (0, 1, 0, "source.control-flow.cpp-v1"),
        ("js/control.js", "jsFlow"): (11, 12, 3, "source.control-flow.javascript-v1"),
        ("js/control.js", "jsBound"): (1, 2, 1, "source.control-flow.javascript-v1"),
        ("js/control.js", "jsOuter"): (0, 1, 0, "source.control-flow.javascript-v1"),
        ("ts/control.ts", "tsFlow"): (4, 5, 2, "source.control-flow.typescript-v1"),
        ("tsx/control.tsx", "View"): (2, 3, 2, "source.control-flow.tsx-v1"),
        ("py/control.py", "py_flow"): (12, 13, 3, "source.control-flow.python-v1"),
        ("py/control.py", "py_outer"): (0, 1, 0, "source.control-flow.python-v1"),
        ("r/control.R", "r_flow"): (10, 11, 3, "source.control-flow.r-v1"),
        ("r/control.R", "r_outer"): (0, 1, 0, "source.control-flow.r-v1"),
    }
    for identity, (decisions, cyclomatic, nesting, definition) in (
        expected_complexity.items()
    ):
        metrics = complexity_symbols[identity]["source_metrics"]
        assert metrics["definition"] == definition
        assert metrics["fact_id"].startswith("f:")
        assert metrics["provider"].startswith("archbird-tree-sitter-")
        assert metrics["state"] == "current"
        assert metrics["values"] == {
            "source.cyclomatic_complexity": cyclomatic,
            "source.decision_count": decisions,
            "source.max_control_nesting": nesting,
        }
        contributors = metrics["contributors"]
        assert sum(row["contribution"] for row in contributors) == decisions
        assert max(
            (row["control_depth"] for row in contributors), default=0
        ) == nesting
        assert all(
            row["construct"]
            and row["fact_id"]
            and row["provider"].startswith("archbird-tree-sitter-")
            and row["line"] > 0
            and row["span"]["end"] > row["span"]["start"]
            for row in contributors
        )

    for metric, expected_index in (
        ("source.decision_count", 0),
        ("source.cyclomatic_complexity", 1),
        ("source.max_control_nesting", 2),
    ):
        result = json.loads(
            evaluate_projection_json(
                complexity_map,
                {
                    "id": metric,
                    "select": "metrics",
                    "metric": metric,
                    "level": "symbol",
                },
            )
        )
        assert result["completeness"]["classification"] == "complete"
        actual = {
            (item["attributes"]["path"], item["attributes"]["symbol"]): item
            for item in result["fact"]["items"]
        }
        assert set(actual) == set(expected_complexity)
        for identity, expected in expected_complexity.items():
            item = actual[identity]
            assert item["value"] == expected[expected_index]
            assert item["attributes"]["definition"] == expected[3]
            assert len(item["evidence"]) >= (2 if expected[0] or expected[2] else 1)

    compact_metrics = query_map_markdown(
        complexity_map,
        symbols=["js/control.js:jsFlow"],
        view="metrics",
        detail="compact",
    ).decode()
    standard_metrics = query_map_markdown(
        complexity_map,
        symbols=["js/control.js:jsFlow"],
        view="metrics",
        detail="standard",
    ).decode()
    full_metrics = query_map_markdown(
        complexity_map,
        symbols=["js/control.js:jsFlow"],
        view="metrics",
        detail="full",
    ).decode()
    metric_line = (
        "`js/control.js:jsFlow`: complexity=12; decisions=11; "
        "max-nesting=3"
    )
    assert metric_line in compact_metrics
    assert "contributors-shown=0" in compact_metrics
    assert metric_line in standard_metrics
    assert "contributors-shown=6" in standard_metrics
    assert "contributors-omitted=7" in standard_metrics
    assert metric_line in full_metrics
    assert "contributors-shown=13" in full_metrics
    assert "contributors-omitted=0" in full_metrics
    assert "span=33..167" in full_metrics
    assert "provider=archbird-tree-sitter-javascript" in full_metrics
    assert "; fact=`f:" in full_metrics
    budgeted_metrics = query_map_markdown(
        complexity_map,
        symbols=["js/control.js:jsFlow"],
        view="metrics",
        detail="standard",
        max_chars=2_000,
    ).decode()
    assert "# Focused source metrics: source-complexity" in budgeted_metrics
    assert len(budgeted_metrics) <= 2_000
    assert "Ranked neighborhood" not in budgeted_metrics

    complexity_policy = {
        "project": "source-complexity",
        "projections": {
            "complexity": {
                "select": "metrics",
                "metric": "source.cyclomatic_complexity",
                "level": "symbol",
            }
        },
        "constraints": {
            "MAX-COMPLEXITY": {
                "assert": "numeric_bounds",
                "actual": {"projection": "complexity"},
                "max": 11,
                "owner": "architecture",
                "rationale": "Exercise source-metric evidence in Verify.",
            }
        },
    }
    complexity_verification = json.loads(
        evaluate_constraints_json(
            json.dumps(complexity_policy, separators=(",", ":")).encode(),
            complexity_map,
        )
    )
    assert complexity_verification["constraints"][0]["status"] == "fail"
    assert any(
        evidence["path"] == "js/control.js" and evidence["line"] == 2
        for finding in complexity_verification["constraints"][0]["findings"]
        for evidence in finding["evidence"]
    )

    incomplete_complexity = json.loads(complexity_map)
    incomplete_symbol = next(
        symbol
        for file in incomplete_complexity["files"]
        if file["path"] == "js/control.js"
        for symbol in file["symbols"]
        if symbol["name"] == "jsFlow"
    )
    incomplete_symbol["source_metrics"]["state"] = "unknown"
    incomplete_result = json.loads(
        evaluate_projection_json(
            json.dumps(incomplete_complexity, separators=(",", ":")).encode(),
            {
                "id": "incomplete-complexity",
                "select": "metrics",
                "metric": "source.cyclomatic_complexity",
                "level": "symbol",
            },
        )
    )
    assert incomplete_result["completeness"]["classification"] == "incomplete"
    assert next(
        item
        for item in incomplete_result["fact"]["items"]
        if item["attributes"]["path"] == "js/control.js"
    )["state"] == "unknown"

    malformed_complexity = json.loads(complexity_map)
    malformed_symbol = next(
        symbol
        for file in malformed_complexity["files"]
        if file["path"] == "js/control.js"
        for symbol in file["symbols"]
        if symbol["name"] == "jsFlow"
    )
    malformed_symbol["source_metrics"]["contributors"][0]["span"].pop("end")
    try:
        evaluate_projection_json(
            json.dumps(malformed_complexity, separators=(",", ":")).encode(),
            {
                "id": "malformed-complexity",
                "select": "metrics",
                "metric": "source.cyclomatic_complexity",
                "level": "symbol",
            },
        )
    except RuntimeError as error:
        assert "invalid source metric contributor" in str(error)
    else:
        raise AssertionError("malformed source metric contributor was accepted")

    missing_provenance = json.loads(complexity_map)
    missing_provenance_symbol = next(
        symbol
        for file in missing_provenance["files"]
        if file["path"] == "js/control.js"
        for symbol in file["symbols"]
        if symbol["name"] == "jsFlow"
    )
    missing_provenance_symbol["source_metrics"]["contributors"][0].pop(
        "provider"
    )
    try:
        evaluate_projection_json(
            json.dumps(missing_provenance, separators=(",", ":")).encode(),
            {
                "id": "missing-complexity-provenance",
                "select": "metrics",
                "metric": "source.cyclomatic_complexity",
                "level": "symbol",
            },
        )
    except RuntimeError as error:
        assert "invalid source metric contributor" in str(error)
    else:
        raise AssertionError("source metric contributor without provider was accepted")

    inconsistent_complexity = json.loads(complexity_map)
    inconsistent_symbol = next(
        symbol
        for file in inconsistent_complexity["files"]
        if file["path"] == "js/control.js"
        for symbol in file["symbols"]
        if symbol["name"] == "jsFlow"
    )
    inconsistent_symbol["source_metrics"]["values"]["source.decision_count"] += 1
    try:
        evaluate_projection_json(
            json.dumps(inconsistent_complexity, separators=(",", ":")).encode(),
            {
                "id": "inconsistent-complexity-ledger",
                "select": "metrics",
                "metric": "source.cyclomatic_complexity",
                "level": "symbol",
            },
        )
    except RuntimeError as error:
        assert "inconsistent source metric evidence" in str(error)
    else:
        raise AssertionError("inconsistent source metric evidence was accepted")
    try:
        query_map_markdown(
            json.dumps(inconsistent_complexity, separators=(",", ":")).encode(),
            symbols=["js/control.js:jsFlow"],
            view="metrics",
            detail="full",
        )
    except RuntimeError as error:
        assert "source metric evidence is inconsistent" in str(error)
    else:
        raise AssertionError("inconsistent source metrics were rendered")

    report_map = (ROOT / "test/fixtures/report_map.json").read_bytes()
    legacy_extent = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "legacy-source-extents",
                "select": "metrics",
                "metric": "source.extent_lines",
                "level": "symbol",
            },
        )
    )
    assert legacy_extent["completeness"]["classification"] == "unknown"
    assert legacy_extent["fact"]["state"] == "unknown"
    graph_definition = {
        "id": "file-import-graph",
        "select": "graph",
        "level": "file",
        "relations": ["imports"],
    }
    graph = json.loads(evaluate_projection_json(report_map, graph_definition))
    expected: dict[str, set[str]] = {}
    for item in graph["fact"]["items"]:
        attributes = item["attributes"]
        if attributes.get("record_kind") == "relation":
            expected.setdefault(attributes["source"], set()).add(
                attributes["target"]
            )
    neighbors = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "file-out-neighbors",
                "select": "metrics",
                "metric": "graph.out_neighbors",
                "level": "file",
                "relations": ["imports"],
            },
        )
    )
    assert neighbors["completeness"]["classification"] == "complete"
    assert {
        item["attributes"]["node"]: item["value"]
        for item in neighbors["fact"]["items"]
    } == {
        item["attributes"]["id"]: len(expected.get(item["attributes"]["id"], set()))
        for item in graph["fact"]["items"]
        if item["attributes"].get("record_kind") == "node"
    }
    assert all(
        item["attributes"]["definition"] == "graph.directed-neighbors-v1"
        and item["attributes"]["unit"] == "neighbor"
        for item in neighbors["fact"]["items"]
    )

    incoming = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "file-in-neighbors",
                "select": "metrics",
                "metric": "graph.in_neighbors",
                "level": "file",
                "relations": ["imports"],
            },
        )
    )
    assert incoming["completeness"]["classification"] == "complete"
    assert {
        item["attributes"]["node"]: item["value"]
        for item in incoming["fact"]["items"]
    } == {
        item["attributes"]["id"]: len(
            {
                relation["attributes"]["source"]
                for relation in graph["fact"]["items"]
                if relation["attributes"].get("record_kind") == "relation"
                and relation["attributes"]["target"]
                == item["attributes"]["id"]
            }
        )
        for item in graph["fact"]["items"]
        if item["attributes"].get("record_kind") == "node"
    }
    assert all(
        item["attributes"]["definition"]
        == "graph.directed-in-neighbors-v1"
        and item["attributes"]["unit"] == "neighbor"
        for item in incoming["fact"]["items"]
    )

    for metric, direction, expected_definition in (
        ("graph.out_relations", "source", "graph.directed-out-relations-v1"),
        ("graph.in_relations", "target", "graph.directed-in-relations-v1"),
    ):
        result = json.loads(
            evaluate_projection_json(
                report_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "relations": ["imports"],
                },
            )
        )
        expected_relations = {
            node["attributes"]["id"]: sum(
                relation["attributes"]["witness_count"]
                for relation in graph["fact"]["items"]
                if relation["attributes"].get("record_kind") == "relation"
                and relation["attributes"][direction]
                == node["attributes"]["id"]
            )
            for node in graph["fact"]["items"]
            if node["attributes"].get("record_kind") == "node"
        }
        assert {
            item["attributes"]["node"]: item["value"]
            for item in result["fact"]["items"]
        } == expected_relations
        assert all(
            item["attributes"]["definition"] == expected_definition
            and item["attributes"]["unit"] == "relation"
            for item in result["fact"]["items"]
        )

    grouped_graph_definition = {
        "id": "layer-file-graph",
        "select": "graph",
        "group_by": "layer",
        "level": "file",
        "relations": ["bridges", "declarations", "imports"],
        "overlays": ["metrics"],
    }
    grouped_graph = json.loads(
        evaluate_projection_json(report_map, grouped_graph_definition)
    )
    memberships: dict[str, set[str]] = {}
    group_kinds = {
        row["attributes"]["id"]: row["attributes"]["group_by"]
        for row in grouped_graph["fact"]["items"]
        if row["attributes"].get("record_kind") == "group"
    }
    for row in grouped_graph["fact"]["items"]:
        if row["attributes"].get("record_kind") == "membership":
            memberships.setdefault(row["attributes"]["node"], set()).add(
                row["attributes"]["group"]
            )
    expected_boundaries = {}
    for row in grouped_graph["fact"]["items"]:
        if row["attributes"].get("record_kind") != "relation":
            continue
        attributes = row["attributes"]
        count = attributes["witness_count"]
        for source_group in memberships[attributes["source"]]:
            for target_group in memberships[attributes["target"]]:
                if group_kinds[source_group] != group_kinds[target_group]:
                    continue
                source_counts = expected_boundaries.setdefault(
                    source_group,
                    {
                        "internal": 0,
                        "inbound": 0,
                        "outbound": 0,
                        "inbound_groups": set(),
                        "outbound_groups": set(),
                    },
                )
                target_counts = expected_boundaries.setdefault(
                    target_group,
                    {
                        "internal": 0,
                        "inbound": 0,
                        "outbound": 0,
                        "inbound_groups": set(),
                        "outbound_groups": set(),
                    },
                )
                if source_group == target_group:
                    source_counts["internal"] += count
                else:
                    source_counts["outbound"] += count
                    target_counts["inbound"] += count
                    source_counts["outbound_groups"].add(target_group)
                    target_counts["inbound_groups"].add(source_group)
    for metric, key, expected_definition in (
        (
            "graph.internal_relations",
            "internal",
            "graph.group-internal-relations-v1",
        ),
        (
            "graph.inbound_relations",
            "inbound",
            "graph.group-inbound-relations-v1",
        ),
        (
            "graph.outbound_relations",
            "outbound",
            "graph.group-outbound-relations-v1",
        ),
    ):
        result = json.loads(
            evaluate_projection_json(
                report_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "group_by": "layer",
                    "relations": ["bridges", "declarations", "imports"],
                },
            )
        )
        assert {
            item["attributes"]["group"]: item["value"]
            for item in result["fact"]["items"]
        } == {
            row["attributes"]["id"]: expected_boundaries.get(
                row["attributes"]["id"],
                {
                    "internal": 0,
                    "inbound": 0,
                    "outbound": 0,
                    "inbound_groups": set(),
                    "outbound_groups": set(),
                },
            )[key]
            for row in grouped_graph["fact"]["items"]
            if row["attributes"].get("record_kind") == "group"
            and row["attributes"].get("group_by") == "layer"
        }
        assert all(
            item["attributes"]["definition"] == expected_definition
            and item["attributes"]["unit"] == "relation"
            and item["attributes"]["group_by"] == "layer"
            for item in result["fact"]["items"]
        )

    for metric, key, expected_definition in (
        (
            "graph.inbound_group_neighbors",
            "inbound_groups",
            "graph.group-inbound-neighbors-v1",
        ),
        (
            "graph.outbound_group_neighbors",
            "outbound_groups",
            "graph.group-outbound-neighbors-v1",
        ),
    ):
        result = json.loads(
            evaluate_projection_json(
                report_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "group_by": "layer",
                    "relations": ["bridges", "declarations", "imports"],
                },
            )
        )
        assert {
            item["attributes"]["group"]: item["value"]
            for item in result["fact"]["items"]
        } == {
            row["attributes"]["id"]: len(
                expected_boundaries.get(
                    row["attributes"]["id"],
                    {"inbound_groups": set(), "outbound_groups": set()},
                )[key]
            )
            for row in grouped_graph["fact"]["items"]
            if row["attributes"].get("record_kind") == "group"
            and row["attributes"].get("group_by") == "layer"
        }
        assert all(
            item["attributes"]["definition"] == expected_definition
            and item["attributes"]["unit"] == "group"
            and item["attributes"]["group_by"] == "layer"
            for item in result["fact"]["items"]
        )

    cyclic_map = json.loads(report_map)
    cyclic_map["edges"].extend(
        [
            {
                "kind": "import",
                "names": ["./index.js"],
                "source": "js/runtime.js",
                "target": "js/index.js",
            },
            {
                "kind": "import",
                "names": ["./worker.js"],
                "source": "js/worker.js",
                "target": "js/worker.js",
            },
            {
                "kind": "import",
                "names": ["../native/addon.c"],
                "source": "js/main.js",
                "target": "native/addon.c",
            },
            {
                "kind": "import",
                "names": ["../py/sample/api.py"],
                "source": "js/main.js",
                "target": "py/sample/api.py",
            },
            {
                "kind": "import",
                "names": ["../src/core.c"],
                "source": "js/main.js",
                "target": "src/core.c",
            },
        ]
    )
    encoded_cyclic_map = json.dumps(
        cyclic_map, separators=(",", ":"), sort_keys=True
    ).encode()
    expected_cycles = {
        "file:js/index.js": (2, 1),
        "file:js/runtime.js": (2, 1),
        "file:js/worker.js": (1, 1),
    }
    for metric, expected_definition, expected_unit, expected_index in (
        ("graph.scc_size", "graph.strongly-connected-component-size-v1", "node", 0),
        ("graph.cyclic", "graph.cycle-membership-v1", "boolean", 1),
    ):
        result = json.loads(
            evaluate_projection_json(
                encoded_cyclic_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "relations": ["imports"],
                },
            )
        )
        values = {
            item["attributes"]["node"]: item["value"]
            for item in result["fact"]["items"]
        }
        assert all(
            values[node] == expected[expected_index]
            for node, expected in expected_cycles.items()
        )
        assert values["file:js/main.js"] == (1 if expected_index == 0 else 0)
        assert all(
            item["attributes"]["definition"] == expected_definition
            and item["attributes"]["unit"] == expected_unit
            for item in result["fact"]["items"]
        )
    for metric in ("graph.out_relations", "graph.in_relations"):
        result = json.loads(
            evaluate_projection_json(
                encoded_cyclic_map,
                {
                    "id": f"self-loop-{metric}",
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "relations": ["imports"],
                },
            )
        )
        values = {
            item["attributes"]["node"]: item["value"]
            for item in result["fact"]["items"]
        }
        assert values["file:js/worker.js"] == 1
    cycle_policy = {
        "project": "sample",
        "projections": {
            "import-cycles": {
                "select": "metrics",
                "metric": "graph.cyclic",
                "level": "file",
                "relations": ["imports"],
            }
        },
        "constraints": {
            "NO-IMPORT-CYCLES": {
                "assert": "numeric_bounds",
                "actual": {"projection": "import-cycles"},
                "max": 0,
                "owner": "architecture",
                "rationale": "Exercise cycle membership as a Verify operand.",
            }
        },
    }
    cycle_verification = json.loads(
        evaluate_constraints_json(
            json.dumps(cycle_policy, separators=(",", ":")).encode(),
            encoded_cyclic_map,
        )
    )
    assert cycle_verification["constraints"][0]["status"] == "fail"
    for detail in ("compact", "standard", "full"):
        cycle_report = render_map_markdown(
            encoded_cyclic_map,
            view="metrics",
            detail=detail,
            relations=["imports"],
        )
        assert b"scc-size=2; cyclic=yes; cycle-kind=multi-node" in cycle_report
        assert b"scc-size=1; cyclic=yes; cycle-kind=self-loop" in cycle_report
        assert b"scc-size=1; cyclic=no; cycle-kind=none" in cycle_report
        assert cycle_report.index(b"file:js/index.js") < cycle_report.index(
            b"file:js/main.js"
        )
        assert cycle_report.index(b"file:js/main.js") < cycle_report.index(
            b"file:js/worker.js"
        )
    incomplete_cycle_report = render_map_markdown(
        encoded_cyclic_map, view="metrics", detail="standard"
    )
    assert (
        b"scc-size=>=2; cyclic=yes; cycle-kind=multi-node"
        in incomplete_cycle_report
    )
    assert b"cycle-kind=unknown" in incomplete_cycle_report

    # Relation kinds and duplicate witnesses do not multiply one neighbor.
    duplicate_map = json.loads(report_map)
    duplicate_map["edges"].append(
        {
            "kind": "declaration",
            "names": ["duplicate-route"],
            "source": "js/index.js",
            "target": "js/runtime.js",
        }
    )
    duplicate_neighbors = json.loads(
        evaluate_projection_json(
            json.dumps(duplicate_map, separators=(",", ":")).encode(),
            {
                "id": "deduplicated-file-neighbors",
                "select": "metrics",
                "metric": "graph.out_neighbors",
                "level": "file",
                "relations": ["imports", "declarations"],
            },
        )
    )
    duplicate_values = {
        item["attributes"]["node"]: item["value"]
        for item in duplicate_neighbors["fact"]["items"]
    }
    assert duplicate_values["file:js/index.js"] == 1
    duplicate_relations = json.loads(
        evaluate_projection_json(
            json.dumps(duplicate_map, separators=(",", ":")).encode(),
            {
                "id": "multiplicity-file-relations",
                "select": "metrics",
                "metric": "graph.out_relations",
                "level": "file",
                "relations": ["imports", "declarations"],
            },
        )
    )
    duplicate_relation_values = {
        item["attributes"]["node"]: item["value"]
        for item in duplicate_relations["fact"]["items"]
    }
    assert duplicate_relation_values["file:js/index.js"] == 2

    test_metric_map = json.loads(report_map)
    test_metric_map["tests"][0]["inventory_source"] = "discovery"
    test_metric_map["tests"][0]["inventory_state"] = "candidate"
    test_metric_map["tests"][0]["cases"][0]["route_evidence"] = [
        {
            "claim": "symbol-hit",
            "enclosing": "",
            "fact_id": "",
            "hits": 2,
            "line": 0,
            "name": "ops.add",
            "observation_sha256": "a" * 64,
            "provenance": "observed",
            "provider": "fixture-runner",
            "relation": "observed-symbol-hit",
            "scope": "case",
            "span": {"end": 0, "start": 0},
            "target": "js/runtime.js",
            "target_symbol": "ops.add",
        }
    ]
    # A valid observation must not conceal a malformed later evidence row.
    invalid_observation_map = json.loads(json.dumps(test_metric_map))
    invalid_observation_map["tests"][0]["cases"][0]["route_evidence"].append(
        {"provenance": 1}
    )
    try:
        evaluate_projection_json(
            json.dumps(
                invalid_observation_map, separators=(",", ":"), sort_keys=True
            ).encode(),
            {
                "id": "invalid-observed-tests",
                "select": "metrics",
                "metric": "tests.observed_cases",
                "level": "map",
            },
        )
    except RuntimeError as error:
        assert "invalid observed test route evidence" in str(error)
    else:
        raise AssertionError("malformed evidence after an observation was accepted")
    test_metric_map["tests"].append(
        {
            "cases": [
                {
                    "configured_routes": [],
                    "line": 1,
                    "route_evidence": [],
                    "routes": {},
                    "selector": "unit:first",
                },
                {
                    "configured_routes": [],
                    "line": 2,
                    "route_evidence": [],
                    "routes": {},
                    "selector": "unit:second",
                },
            ],
            "count": 2,
            "framework": "fixture",
            "group": "unit",
            "language": "javascript",
            "path": "test/test_unit.js",
            "route_evidence": [],
            "routes": {},
            "selectors": ["unit:first", "unit:second"],
        }
    )
    encoded_test_metric_map = json.dumps(
        test_metric_map, separators=(",", ":"), sort_keys=True
    ).encode()
    for metric, expected, denominator, denominator_unit in (
        ("tests.static_case_occurrences", 3, 3, "static_case_occurrence"),
        ("tests.candidate_case_occurrences", 1, 3, "static_case_occurrence"),
        ("tests.routed_cases", 1, 3, "test_case"),
        ("tests.observed_cases", 1, 3, "test_case"),
    ):
        result = json.loads(
            evaluate_projection_json(
                encoded_test_metric_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "map",
                },
            )
        )
        assert result["completeness"]["classification"] == "complete"
        assert len(result["fact"]["items"]) == 1
        item = result["fact"]["items"][0]
        assert item["value"] == expected
        assert item["attributes"]["denominator"] == denominator
        assert item["attributes"]["denominator_unit"] == denominator_unit
        assert item["attributes"]["metric"] == metric

    evidence_metric_map = json.loads(report_map)
    evidence_metric_map["edges"] = [
        {
            "kind": "import",
            "names": ["current"],
            "source": "js/index.js",
            "target": "js/runtime.js",
        },
        {
            "evidence": [
                {"basis": "heuristic", "provider": "fixture", "state": "unknown"}
            ],
            "kind": "import",
            "names": ["unknown"],
            "source": "js/main.js",
            "target": "js/worker.js",
        },
        {
            "evidence": [
                {"basis": "snapshot", "provider": "fixture", "state": "stale"}
            ],
            "kind": "import",
            "names": ["stale"],
            "source": "js/worker.js",
            "target": "js/index.js",
        },
        {
            "kind": "import",
            "names": ["current-self"],
            "source": "js/index.js",
            "target": "js/index.js",
        },
    ]
    encoded_evidence_metric_map = json.dumps(
        evidence_metric_map, separators=(",", ":"), sort_keys=True
    ).encode()
    for metric, expected in (
        ("evidence.current_relations", 2),
        ("evidence.unknown_relations", 1),
        ("evidence.stale_relations", 1),
    ):
        result = json.loads(
            evaluate_projection_json(
                encoded_evidence_metric_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "relations": ["imports"],
                },
            )
        )
        assert result["completeness"]["classification"] == "incomplete"
        item = result["fact"]["items"][0]
        assert item["value"] == expected
        assert item["attributes"]["denominator"] == 4
        assert item["attributes"]["unit"] == "relation_evidence_record"
        assert item["attributes"]["denominator_unit"] == "relation_evidence_record"
    component_evidence_map = json.loads(report_map)
    component_evidence_map["components"] = [
        {
            "description": "",
            "files": ["js/index.js", "js/main.js"],
            "name": "frontend-a",
            "outgoing": {},
            "symbol_count": 0,
        },
        {
            "description": "",
            "files": ["js/runtime.js", "js/worker.js"],
            "name": "frontend-b",
            "outgoing": {},
            "symbol_count": 0,
        },
    ]
    component_evidence_map["edges"] = evidence_metric_map["edges"][:2]
    encoded_component_evidence_map = json.dumps(
        component_evidence_map, separators=(",", ":"), sort_keys=True
    ).encode()
    for metric, expected in (
        ("evidence.current_relations", 1),
        ("evidence.unknown_relations", 1),
    ):
        result = json.loads(
            evaluate_projection_json(
                encoded_component_evidence_map,
                {
                    "id": f"component-{metric.replace('.', '-')}",
                    "select": "metrics",
                    "metric": metric,
                    "level": "component",
                    "relations": ["imports"],
                },
            )
        )
        item = result["fact"]["items"][0]
        assert item["value"] == expected
        assert item["attributes"]["denominator"] == 2
    evidence_policy = {
        "project": "sample",
        "projections": {
            "unknown-imports": {
                "select": "metrics",
                "metric": "evidence.unknown_relations",
                "level": "file",
                "relations": ["imports"],
            }
        },
        "constraints": {
            "NO-UNKNOWN-IMPORTS": {
                "assert": "numeric_bounds",
                "actual": {"projection": "unknown-imports"},
                "max": 0,
                "owner": "architecture",
                "rationale": "Exercise incomplete evidence accounting.",
            }
        },
    }
    evidence_verification = json.loads(
        evaluate_constraints_json(
            json.dumps(evidence_policy, separators=(",", ":")).encode(),
            encoded_evidence_metric_map,
        )
    )
    assert evidence_verification["constraints"][0]["status"] == "unknown"

    resolution_metric_map = json.loads(fixture_map)
    unique_call = resolution_metric_map["symbol_calls"][0]
    candidate_call = json.loads(json.dumps(unique_call))
    candidate_call["name"] = "candidate-call"
    candidate_call["resolution"] = "candidate"
    ambiguous_call = json.loads(json.dumps(unique_call))
    ambiguous_call["name"] = "ambiguous-call"
    ambiguous_call["resolution"] = "ambiguous"
    ambiguous_call["candidates"] = [
        unique_call["candidates"][0],
        resolution_metric_map["symbol_calls"][1]["candidates"][0],
    ]
    unresolved_call = json.loads(json.dumps(unique_call))
    unresolved_call["name"] = "unresolved-call"
    unresolved_call["resolution"] = "unresolved"
    unresolved_call["candidates"] = []
    resolution_metric_map["symbol_calls"].extend(
        [candidate_call, ambiguous_call, unresolved_call]
    )
    encoded_resolution_metric_map = json.dumps(
        resolution_metric_map, separators=(",", ":"), sort_keys=True
    ).encode()
    for metric, expected in (
        ("evidence.candidate_relations", 1),
        ("evidence.ambiguous_relations", 2),
        ("evidence.unresolved_relations", 1),
    ):
        result = json.loads(
            evaluate_projection_json(
                encoded_resolution_metric_map,
                {
                    "id": metric.replace(".", "-"),
                    "select": "metrics",
                    "metric": metric,
                    "level": "symbol",
                    "relations": ["calls"],
                },
            )
        )
        item = result["fact"]["items"][0]
        assert item["value"] == expected
        assert item["attributes"]["denominator"] == 6
        assert item["attributes"]["unit"] == "relation_evidence_record"
        assert item["attributes"]["denominator_unit"] == "relation_evidence_record"
    resolution_report = render_map_markdown(
        encoded_resolution_metric_map,
        view="metrics",
        detail="standard",
        relations=["calls"],
    )
    assert b"uncertain-resolution: candidate=>=1" in resolution_report
    assert b"ambiguous=>=2" in resolution_report
    assert b"unresolved=>=1" in resolution_report

    incomplete = json.loads(
        evaluate_projection_json(
            report_map,
            {
                "id": "incomplete-neighbors",
                "select": "metrics",
                "metric": "graph.out_neighbors",
                "level": "file",
                "relations": ["calls"],
            },
        )
    )
    assert incomplete["completeness"]["classification"] == "incomplete"
    assert all(item["state"] == "unknown" for item in incomplete["fact"]["items"])
    assert all(item["value"] is None for item in incomplete["fact"]["items"])
    for metric in (
        "graph.in_neighbors",
        "graph.out_relations",
        "graph.in_relations",
        "graph.scc_size",
        "graph.cyclic",
    ):
        incomplete_metric = json.loads(
            evaluate_projection_json(
                report_map,
                {
                    "id": f"incomplete-{metric}",
                    "select": "metrics",
                    "metric": metric,
                    "level": "file",
                    "relations": ["calls"],
                },
            )
        )
        assert incomplete_metric["completeness"]["classification"] == "incomplete"
        assert all(
            item["state"] == "unknown"
            for item in incomplete_metric["fact"]["items"]
        )
        assert all(item["value"] is None for item in incomplete_metric["fact"]["items"])

    report = render_map_markdown(report_map, view="metrics", detail="standard")
    assert report.startswith(b"# sample code metrics\n")
    assert b"## Metric definitions\n" in report
    assert b"source.extent_lines" in report
    assert b"graph.out_neighbors" in report
    assert b"graph.in_neighbors" in report
    assert b"graph.out_relations" in report
    assert b"graph.in_relations" in report
    assert b"graph.internal_relations" in report
    assert b"graph.inbound_relations" in report
    assert b"graph.outbound_relations" in report
    assert b"graph.inbound_group_neighbors" in report
    assert b"graph.outbound_group_neighbors" in report
    assert b"graph.scc_size" in report
    assert b"graph.cyclic" in report
    assert b"## Metric distributions\n" in report
    assert b"quantiles=unavailable (incomplete population)" in report
    assert b"## Relation evidence\n" in report
    assert b"Source extent metrics are unavailable" in report
    assert b"out-neighbors=>=1; in-neighbors=>=0" in report
    assert b"scc-size=>=1; cyclic=unknown (no known cycle)" in report
    assert b"internal-relations=>=1; inbound-relations=>=1" in report
    assert b"boundary-fraction=" in report
    assert b"Evidence: metrics=incomplete" in report
    complete_graph_report = render_map_markdown(
        report_map, view="metrics", detail="standard", relations=["imports"]
    )
    assert b"out-neighbors=1; in-neighbors=0" in complete_graph_report
    assert b"scc-size=1; cyclic=no" in complete_graph_report
    assert b"out-neighbors=>=" not in complete_graph_report
    assert b"median=" in complete_graph_report
    assert b"p90=" in complete_graph_report
    assert b"p95=" in complete_graph_report
    assert b"cycle-burden: cyclic-nodes=" in complete_graph_report
    assert b"boundary-fraction=not-applicable" in complete_graph_report
    assert report == _native.map_markdown_view(report_map, 4, 1, max_chars=0)


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
    unresolved_graph = json.loads(
        evaluate_projection_json(
            json.dumps(
                unresolved_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            {
                "id": "unresolved-import-graph",
                "group_by": "directory",
                "level": "file",
                "relations": ["imports"],
                "select": "graph",
            },
        )
    )
    unresolved_nodes = {
        row["attributes"]["id"]: row
        for row in unresolved_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "node"
    }
    assert "unresolved:unresolved-import" in unresolved_nodes
    assert (
        unresolved_nodes["unresolved:unresolved-import"]["label"]
        == "Unresolved imports"
    )
    assert "external:unresolved-import" not in unresolved_nodes

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

    cross_layer = root / "cross-layer-external"
    (cross_layer / "app").mkdir(parents=True)
    (cross_layer / "nested").mkdir()
    (cross_layer / "package_root").mkdir()
    (cross_layer / "app/main.py").write_text(
        "from requests import DEFAULT, get\n\n"
        "def fetch():\n"
        "    get('https://example.invalid')\n"
        "    return DEFAULT\n",
        encoding="utf-8",
    )
    (cross_layer / "package_root/__init__.py").write_text("", encoding="utf-8")
    (cross_layer / "pyproject.toml").write_text(
        "[project]\n"
        'name = "cross-layer-external"\n'
        'version = "1.0.0"\n'
        'dependencies = ["requests>=2"]\n',
        encoding="utf-8",
    )
    (cross_layer / "nested/main.py").write_text(
        "import httpx\nimport requests\n", encoding="utf-8"
    )
    (cross_layer / "nested/pyproject.toml").write_text(
        "[project]\n"
        'name = "nested-package"\n'
        'version = "1.0.0"\n'
        'dependencies = ["httpx>=0.27"]\n',
        encoding="utf-8",
    )
    cross_layer_config = {
        "project": "cross-layer-external",
        "layers": [
            {
                "globs": ["package_root/**/*.py"],
                "language": "python",
                "name": "package",
            },
            {
                "globs": ["app/**/*.py"],
                "language": "python",
                "name": "application",
            },
            {
                "globs": ["nested/**/*.py"],
                "language": "python",
                "name": "nested-application",
            },
        ],
        "packages": [
            {
                "kind": "python",
                "layer": "package",
                "name": "python-package",
                "path": "pyproject.toml",
            },
            {
                "kind": "python",
                "layer": "nested-application",
                "name": "nested-python-package",
                "path": "nested/pyproject.toml",
            },
        ],
    }
    cross_layer_map = json.loads(
        Project.from_repository(
            cross_layer,
            config=json.dumps(cross_layer_config, separators=(",", ":")).encode(),
            jobs=1,
        ).map_json()
    )
    assert any(
        row["kind"] == "external"
        and row["source"] == "app/main.py"
        and row["target"] == "package:requests"
        for row in cross_layer_map["edges"]
    )
    assert not any(
        row["code"] == "unresolved-import" and row["path"] == "app/main.py"
        for row in cross_layer_map["diagnostics"]
    )
    assert any(
        row["kind"] == "external"
        and row["source"] == "nested/main.py"
        and row["target"] == "package:httpx"
        for row in cross_layer_map["edges"]
    )
    assert not any(
        row["kind"] == "external"
        and row["source"] == "nested/main.py"
        and row["target"] == "package:requests"
        for row in cross_layer_map["edges"]
    )
    assert any(
        row["code"] == "unresolved-import"
        and row["path"] == "nested/main.py"
        and row["message"].endswith(": requests")
        for row in cross_layer_map["diagnostics"]
    )
    external_call = next(
        row for row in cross_layer_map["symbol_calls"] if row["name"] == "get"
    )
    assert external_call["resolution"] == "external"
    assert external_call["package"] == "requests"
    external_reference = next(
        row
        for row in cross_layer_map["symbol_references"]
        if row["name"] == "DEFAULT"
    )
    assert external_reference["resolution"] == "external"
    assert external_reference["package"] == "requests"
    external_relations = json.loads(
        evaluate_projection_json(
            json.dumps(
                cross_layer_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            {
                "id": "external-relations",
                "select": "symbol_relations",
            },
        )
    )
    projected_external = [
        row
        for row in external_relations["fact"]["items"]
        if row["attributes"].get("target") == "package:requests"
    ]
    assert {row["attributes"]["names"][0] for row in projected_external} >= {
        "DEFAULT",
        "get",
    }
    assert all(row["state"] == "current" for row in projected_external)
    external_graph = json.loads(
        evaluate_projection_json(
            json.dumps(
                cross_layer_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            {
                "id": "external-graph",
                "level": "file",
                "relations": ["calls", "references"],
                "select": "graph",
            },
        )
    )
    assert any(
        row["attributes"].get("record_kind") == "node"
        and row["attributes"].get("id") == "package:requests"
        for row in external_graph["fact"]["items"]
    )
    assert any(
        row["attributes"].get("record_kind") == "relation"
        and row["attributes"].get("target") == "package:requests"
        and row["state"] == "current"
        for row in external_graph["fact"]["items"]
    )

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

    relative_package = root / "relative-package"
    (relative_package / "pkg").mkdir(parents=True)
    (relative_package / "pkg/__init__.py").write_text(
        "VALUE = 1\n", encoding="utf-8"
    )
    (relative_package / "pkg/helper.py").write_text(
        "def run():\n    return 1\n", encoding="utf-8"
    )
    relative_source = "from . import helper, VALUE\n\nhelper.run()\n"
    (relative_package / "pkg/consumer.py").write_text(
        relative_source, encoding="utf-8"
    )
    relative_config = {
        "project": "relative-package",
        "layers": [
            {
                "globs": ["**/*.py"],
                "import_roots": ["."],
                "language": "python",
                "name": "python",
            }
        ],
    }
    relative_map = json.loads(
        Project.from_repository(
            relative_package,
            config=json.dumps(relative_config, separators=(",", ":")).encode(),
            jobs=1,
        ).map_json()
    )
    assert any(
        row["kind"] == "import"
        and row["source"] == "pkg/consumer.py"
        and row["target"] == "pkg/__init__.py"
        for row in relative_map["edges"]
    )
    helper_edge = next(
        row
        for row in relative_map["edges"]
        if row["kind"] == "import"
        and row["source"] == "pkg/consumer.py"
        and row["target"] == "pkg/helper.py"
    )
    assert helper_edge["names"] == [".helper"]
    helper_start = relative_source.index("helper")
    assert any(
        site["name"] == "helper"
        and site["line"] == 1
        and site["span"] == {
            "start": helper_start,
            "end": helper_start + len("helper"),
        }
        for site in helper_edge["sites"]
    )
    assert not any(
        row["code"] == "unresolved-import"
        for row in relative_map["diagnostics"]
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

    zero_cpp = root / "zero-cpp"
    (zero_cpp / "include/Acme").mkdir(parents=True)
    (zero_cpp / "src").mkdir()
    (zero_cpp / "include/Acme/detail.hpp").write_text(
        "#pragma once\ninline int detail() { return 1; }\n",
        encoding="utf-8",
    )
    (zero_cpp / "include/Acme/detail.h").write_text(
        "#pragma once\n#define ACME_DETAIL 1\n",
        encoding="utf-8",
    )
    (zero_cpp / "src/main.cpp").write_text(
        "#include <Acme/detail.hpp>\nint main() { return detail(); }\n",
        encoding="utf-8",
    )
    (zero_cpp / "src/main.c").write_text(
        "#include <Acme/detail.h>\nint main(void) { return ACME_DETAIL; }\n",
        encoding="utf-8",
    )
    zero_cpp_map = json.loads(
        Project.from_repository(zero_cpp, jobs=1, map_cache=False).map_json()
    )
    assert any(
        row["kind"] == "import"
        and row["source"] == "src/main.cpp"
        and row["target"] == "include/Acme/detail.hpp"
        and row["names"] == ["Acme/detail.hpp"]
        for row in zero_cpp_map["edges"]
    )
    assert any(
        row["kind"] == "import"
        and row["source"] == "src/main.c"
        and row["target"] == "include/Acme/detail.h"
        and row["names"] == ["Acme/detail.h"]
        for row in zero_cpp_map["edges"]
    )
    assert not any(
        row["source"] in {"src/main.c", "src/main.cpp"}
        and row["target"]
        in {"package:Acme/detail.h", "package:Acme/detail.hpp"}
        for row in zero_cpp_map["edges"]
    )


def _assert_projection_markdown_explainability() -> None:
    report_map = json.loads(
        (ROOT / "test/fixtures/report_map.json").read_bytes()
    )
    template = next(
        row for row in report_map["files"] if row["path"] == "js/index.js"
    )

    def add_file(
        path: str, *, layer: str = "javascript", roles: list[str] | None = None
    ) -> None:
        row = json.loads(json.dumps(template))
        row["path"] = path
        row["layer"] = layer
        row["sha256"] = hashlib.sha256(path.encode()).hexdigest()
        report_map["files"].append(row)
        if roles is not None:
            report_map.setdefault("inputs", []).append(
                {
                    "bytes": row["bytes"],
                    "language": row["language"],
                    "layer": layer,
                    "path": path,
                    "roles": sorted({"source", *roles}),
                    "sha256": row["sha256"],
                }
            )

    report_map["layers"].extend(
        [
            {
                "files": 1,
                "language": "javascript",
                "name": "ci-runner",
                "role": "test-runner",
                "symbols": 0,
            },
            {
                "files": 1,
                "language": "javascript",
                "name": "generated-delivery",
                "role": "generated-delivery",
                "symbols": 0,
            },
        ]
    )

    for path in (
        "graphify/build.py",
        "mesonbuild/cmake/common.py",
        "not-tests/testimonial.js",
        "notpublic/wasm/source.js",
    ):
        add_file(path)
    for index in range(10):
        add_file(f"tests/fixtures/hot-{index}.js")
    add_file("ci/runner.js", layer="ci-runner")
    for path in (
        "public/wasm/generated.js",
        "scripts/prepack.js",
        "webpack.config.js",
        "build/output.js",
        "artifacts/release.js",
        "cmake/tool.c",
        "include/library/fmt/bundled/format.h",
    ):
        add_file(path)
    add_file("opaque/bundle.js", layer="generated-delivery")
    add_file("test cases/common/basic.py")
    add_file("manual tests/runner.py")
    add_file("unittests/allplatform.py")
    add_file("runtime-tests/server.test.ts")
    add_file("typing_tests/check_types.py")
    add_file("role-only/specimen.js", roles=["test-candidate"])
    add_file("tests/vendor-source.js", roles=["third-party-candidate"])
    add_file("build/test-source.js", roles=["test-candidate"])
    report_map["files"].sort(key=lambda row: row["path"])
    report_map["inputs"].sort(key=lambda row: row["path"])

    diagnostics = []
    diagnostics.extend(
        {
            "code": "unresolved-import",
            "message": (
                "import is not resolved internally, builtin, or declared "
                "external: numpy"
            ),
            "path": "tests/fixtures/hot-0.js" if index < 5 else "js/index.js",
            "severity": "warning",
        }
        for index in range(7)
    )
    diagnostics.extend(
        {
            "code": "unresolved-import",
            "message": (
                "import is not resolved internally, builtin, or declared "
                "external: sample.local"
            ),
            "path": f"tests/fixtures/hot-{index + 1}.js",
            "severity": "warning",
        }
        for index in range(3)
    )
    diagnostics.extend(
        [
            {
                "code": "tree-sitter-error",
                "message": (
                    "Tree-sitter javascript recovered from 1 ERROR node(s)"
                ),
                "path": "not-tests/testimonial.js",
                "severity": "warning",
            },
            {
                "code": "tree-sitter-missing",
                "message": (
                    "Tree-sitter javascript inserted 1 MISSING node(s)"
                ),
                "path": "notpublic/wasm/source.js",
                "severity": "warning",
            },
        ]
    )
    diagnostics.append(
        {
            "code": "provider-error",
            "message": "provider failed before producing evidence",
            "path": "graphify/build.py",
            "severity": "error",
        }
    )
    report_map["diagnostics"] = diagnostics
    encoded = json.dumps(
        report_map, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode()
    graph_definition = {
        "id": "explainability-regression",
        "select": "graph",
        "group_by": "directory",
        "level": "file",
        "relations": ["builds", "bridges", "imports", "packages", "tests"],
        "overlays": ["diagnostics", "evidence-quality"],
    }
    projection_before = evaluate_projection_json(encoded, graph_definition)
    report = render_map_markdown(encoded, detail="standard").decode()
    assert evaluate_projection_json(encoded, graph_definition) == projection_before
    assert "Selected files by presentation category: production/API=14; " in report
    assert "test/fixture=18; build/artifact=9." in report
    assert (
        "Category shortlist: production/API=6/14; test/fixture=3/18; "
        "build/artifact=3/9."
    ) in report
    assert "### Production and API candidates" in report
    assert "### Tests and fixtures" in report
    assert "### Build and artifact paths" in report
    assert "### Unresolved frontier" in report
    assert "showing 4 of 5 paths" in report
    assert (
        "13 canonical records grouped into 5 exact causes by severity, code, "
        "and message"
    ) in report
    numpy_group = (
        "`unresolved-import` - occurrences=7; paths=2; import is not resolved "
        "internally, builtin, or declared external: numpy"
    )
    assert report.count(numpy_group) == 1
    assert (
        "Representative paths: `tests/fixtures/hot-0.js` (5), "
        "`js/index.js` (2)"
    ) in report
    assert report.count("`layers[].import_roots`") == 1
    assert report.count("`tree-sitter-*`") == 1
    assert report.index("**error** `provider-error`") < report.index(
        "**warning** `unresolved-import`"
    )
    compact = render_map_markdown(encoded, detail="compact")
    assert compact == render_map_markdown(encoded, detail="compact")
    full = render_map_markdown(encoded, detail="full")
    assert b"canonical records grouped into" not in full
    assert full.count(b"`unresolved-import`") == 10
    bounded = render_map_markdown(encoded, detail="standard", max_chars=3_500)
    assert len(bounded.decode()) <= 3_500

    for path, roles, expected in (
        ("mesonbuild/cmake/common.py", [], "production/API=11; test/fixture=0; build/artifact=0"),
        ("test cases/common/basic.py", [], "production/API=10; test/fixture=1; build/artifact=0"),
        ("manual tests/runner.py", [], "production/API=10; test/fixture=1; build/artifact=0"),
        ("unittests/allplatform.py", [], "production/API=10; test/fixture=1; build/artifact=0"),
        ("runtime-tests/server.test.ts", [], "production/API=10; test/fixture=1; build/artifact=0"),
        ("typing_tests/check_types.py", [], "production/API=10; test/fixture=1; build/artifact=0"),
        (
            "role-only/specimen.js",
            ["test-candidate"],
            "production/API=10; test/fixture=1; build/artifact=0",
        ),
        (
            "role-only/library.js",
            ["artifact-input"],
            "production/API=11; test/fixture=0; build/artifact=0",
        ),
        (
            "role-only/build.js",
            ["build"],
            "production/API=10; test/fixture=0; build/artifact=1",
        ),
        (
            "role-only/generated.js",
            ["generated-candidate"],
            "production/API=10; test/fixture=0; build/artifact=1",
        ),
        (
            "tests/vendor-source.js",
            ["third-party-candidate"],
            "production/API=10; test/fixture=0; build/artifact=1",
        ),
        (
            "build/test-source.js",
            ["test-candidate"],
            "production/API=10; test/fixture=1; build/artifact=0",
        ),
        (
            "vendor/tests/combined.js",
            ["test-candidate", "third-party-candidate"],
            "production/API=10; test/fixture=0; build/artifact=1",
        ),
        (
            "cmake/tool.c",
            [],
            "production/API=10; test/fixture=0; build/artifact=1",
        ),
        (
            "include/library/fmt/bundled/format.h",
            [],
            "production/API=10; test/fixture=0; build/artifact=1",
        ),
    ):
        classification_map = json.loads(
            (ROOT / "test/fixtures/report_map.json").read_bytes()
        )
        classification_row = json.loads(json.dumps(template))
        classification_row["path"] = path
        classification_row["sha256"] = hashlib.sha256(path.encode()).hexdigest()
        classification_map["files"].append(classification_row)
        classification_map["files"].sort(key=lambda row: row["path"])
        if roles:
            classification_map["inputs"] = [
                {
                    "bytes": classification_row["bytes"],
                    "language": classification_row["language"],
                    "layer": classification_row["layer"],
                    "path": path,
                    "roles": sorted({"source", *roles}),
                    "sha256": classification_row["sha256"],
                }
            ]
        classification_report = render_map_markdown(
            json.dumps(
                classification_map,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode(),
            detail="standard",
        ).decode()
        assert f"Selected files by presentation category: {expected}." in (
            classification_report
        ), path

    diagnostic_map = json.loads(json.dumps(report_map))
    diagnostic_map["diagnostics"] = [
        {
            "code": "tree-sitter-error",
            "message": f"Tree-sitter javascript recovered from {nodes} ERROR node(s)",
            "path": path,
            "severity": "warning",
        }
        for nodes, path in ((1, "js/index.js"), (7, "js/main.js"), (7, "js/main.js"))
    ] + [
        {
            "code": "tree-sitter-missing",
            "message": "Tree-sitter javascript inserted 2 MISSING node(s)",
            "path": "js/runtime.js",
            "severity": "warning",
        },
        {
            "code": "tree-sitter-error",
            "message": "Tree-sitter javascript recovered from many ERROR node(s)",
            "path": "js/worker.js",
            "severity": "warning",
        },
        {
            "code": "tree-sitter-error",
            "message": "Tree-sitter javascript recovered from 01 ERROR node(s)",
            "path": "js/worker.js",
            "severity": "warning",
        },
    ]
    diagnostic_encoded = json.dumps(
        diagnostic_map, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode()
    diagnostic_projection_before = evaluate_projection_json(
        diagnostic_encoded, graph_definition
    )
    diagnostic_report = render_map_markdown(
        diagnostic_encoded, detail="standard"
    ).decode()
    assert (
        evaluate_projection_json(diagnostic_encoded, graph_definition)
        == diagnostic_projection_before
    )
    assert (
        "6 canonical records across 5 exact causes; summarized into 4 review "
        "groups"
    ) in diagnostic_report
    assert (
        "occurrences=3; paths=2; exact-causes=2; Tree-sitter javascript parser "
        "recovery; ERROR nodes=15 total; per-record=1..7"
    ) in diagnostic_report
    assert "Tree-sitter javascript recovered from many ERROR node(s)" in (
        diagnostic_report
    )
    assert "Tree-sitter javascript recovered from 01 ERROR node(s)" in (
        diagnostic_report
    )
    diagnostic_full = render_map_markdown(diagnostic_encoded, detail="full")
    assert diagnostic_full.count(b"`tree-sitter-error`") == 5

    many_kind_map = json.loads(json.dumps(report_map))
    many_kind_map["packages"][0]["entrypoints"].update(
        {f"exports:./feature-{index:02d}": "js/index.js" for index in range(20)}
    )
    many_kind_encoded = json.dumps(
        many_kind_map, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode()
    many_kind_projection = json.loads(
        evaluate_projection_json(many_kind_encoded, graph_definition)
    )
    package_flow = next(
        row
        for row in many_kind_projection["fact"]["items"]
        if row["attributes"].get("record_kind") == "group_relation"
        and row["attributes"].get("family") == "packages"
        and len(row["attributes"].get("relation_kinds", [])) > 8
    )
    kind_count = len(package_flow["attributes"]["relation_kinds"])
    many_kind_report = render_map_markdown(
        many_kind_encoded, detail="standard"
    ).decode()
    assert f"+{kind_count - 8} more ({kind_count} total)" in many_kind_report
    assert max(map(len, many_kind_report.splitlines())) < 1_000
    assert "exports:./feature-19" not in many_kind_report
    many_kind_full = render_map_markdown(many_kind_encoded, detail="full").decode()
    assert "packages/exports:./feature-19" in many_kind_full


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
            sys.executable,
            "-m",
            "archbird",
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
        "py/attribute.py": "import api\nresult = api.old_api(4)\n",
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
        "unsupported.go": "package fixture\n\nfunc old_api() {}\n",
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
    map_document = json.loads(map_json)
    alias_edge = next(
        edge
        for edge in map_document["edges"]
        if edge["source"] == "py/alias.py"
        and edge["target"] == "py/api.py"
        and edge["kind"] == "imported-call"
    )
    assert alias_edge["names"] == ["old_api"]
    assert [site["name"] for site in alias_edge["sites"]] == ["kept_alias"]

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
        ("py/attribute.py", "reference", "current"),
        ("py/shadow.py", "import", "current"),
        ("py/use.py", "import", "current"),
        ("py/use.py", "reference", "current"),
    }
    unsupported_map = Project.from_repository(
        root, config=b"{}", jobs=1, map_cache=False
    ).map_json()
    unsupported_projection = json.loads(
        evaluate_projection_json(
            unsupported_map,
            {
                "id": "unsupported-occurrences",
                "names": ["old_api"],
                "paths": ["py/api.py"],
                "select": "symbol_occurrences",
            },
        )
    )
    assert unsupported_projection["completeness"]["classification"] == "incomplete"
    assert unsupported_projection["completeness"]["counts"]["unsupported"] == 1
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


def _assert_ordered_container_facts() -> None:
    root = ROOT / "build/ordered-container-facts"
    shutil.rmtree(root, ignore_errors=True)
    (root / "py").mkdir(parents=True)
    (root / "c").mkdir()
    (root / "py/rules.py").write_text(
        "module_rules = [(\"a\", callback_a), (\"b\", callback_b)]\n"
        "\n"
        "def build():\n"
        "    function_rules = PatternMatcher([\n"
        "        (\"x\", callback_x),\n"
        "        (\"x\", callback_x),\n"
        "    ])\n"
        "    dynamic_rules = [*function_rules]\n"
        "    comprehension_rules = [item for item in function_rules]\n"
        "    composed_rules = function_rules + PatternMatcher([('z', callback_x)])\n"
        "    mutated_rules = [(\"initial\", callback_x)]\n"
        "    mutated_rules.append((\"later\", callback_x))\n"
        "    return (function_rules, dynamic_rules, comprehension_rules, "
        "mutated_rules)\n",
        encoding="utf-8",
    )
    (root / "c/rules.c").write_text(
        "typedef struct { int pattern; void (*callback)(void); } Rule;\n"
        "static void callback_a(void) {}\n"
        "static void callback_b(void) {}\n"
        "#define EXPAND_RULES {4, callback_a}, {5, callback_b}\n"
        "static Rule global_rules[] = {{0, callback_a}};\n"
        "enum CallbackId { CALLBACK_A, CALLBACK_B };\n"
        "static int callbacks[] = {CALLBACK_A, CALLBACK_B};\n"
        "static int make_value(void) { return 1; }\n"
        "static void runtime(void) {\n"
        "  int values[] = {make_value()};\n"
        "  (void)values;\n"
        "}\n"
        "void first(void) {\n"
        "  Rule rules[] = {{1, callback_a}, {2, callback_b}};\n"
        "  (void)rules;\n"
        "}\n"
        "void second(void) {\n"
        "  Rule rules[] = {{3, callback_b}};\n"
        "  (void)rules;\n"
        "}\n"
        "void expanded(void) {\n"
        "  Rule rules[] = {EXPAND_RULES};\n"
        "  (void)rules;\n"
        "}\n",
        encoding="utf-8",
    )
    configuration = {
        "project": "ordered-fixture",
        "layers": [
            {"globs": ["py/**/*.py"], "language": "python", "name": "python"},
            {"globs": ["c/**/*.c"], "language": "c", "name": "c"},
        ],
    }
    unselected = Project.from_repository(
        root,
        config=json.dumps(configuration, separators=(",", ":")).encode(),
        jobs=1,
        map_cache=False,
    )
    assert not any(
        row["domain"] in {"ordered-containers", "ordered-rows"}
        for row in json.loads(unselected.map_json())["facts"]
    )
    configuration["ordered_containers"] = [
        {
            "language": "c",
            "names": ["callbacks", "global_rules", "*/rules", "*/values"],
            "paths": ["c/**/*.c"],
        },
        {
            "language": "python",
            "names": ["module_rules", "build/*_rules"],
            "paths": ["py/**/*.py"],
        },
    ]
    project = Project.from_repository(
        root,
        config=json.dumps(configuration, separators=(",", ":")).encode(),
        jobs=1,
        map_cache=False,
    )
    document = json.loads(project.map_json())
    containers = [
        row for row in document["facts"] if row["domain"] == "ordered-containers"
    ]
    assert {
        row["attributes"]["container"]: (
            row["attributes"]["declared_count"],
            row["attributes"]["population_state"],
        )
        for row in containers
    } == {
        "c/rules.c:callbacks": (2, "closed"),
        "c/rules.c:expanded/rules": (0, "dynamic"),
        "c/rules.c:first/rules": (2, "closed"),
        "c/rules.c:global_rules": (1, "closed"),
        "c/rules.c:runtime/values": (1, "closed"),
        "c/rules.c:second/rules": (1, "closed"),
        "py/rules.py:build/function_rules": (2, "closed"),
        "py/rules.py:build/comprehension_rules": (0, "dynamic"),
        "py/rules.py:build/composed_rules": (1, "dynamic"),
        "py/rules.py:build/dynamic_rules": (1, "dynamic"),
        "py/rules.py:build/mutated_rules": (1, "dynamic"),
        "py/rules.py:module_rules": (2, "closed"),
    }
    composed = next(
        row
        for row in containers
        if row["attributes"]["container"]
        == "py/rules.py:build/composed_rules"
    )
    assert composed["attributes"]["incomplete_reason"] == "unsupported-composition"
    rows = [row for row in document["facts"] if row["domain"] == "ordered-rows"]
    first_rows = [
        row
        for row in rows
        if row["attributes"]["container"] == "c/rules.c:first/rules"
    ]
    assert [row["attributes"]["ordinal"] for row in first_rows] == [0, 1]
    function_rows = [
        row
        for row in rows
        if row["attributes"]["container"]
        == "py/rules.py:build/function_rules"
    ]
    assert [row["attributes"]["ordinal"] for row in function_rows] == [0, 1]
    assert len({row["attributes"]["digest"] for row in function_rows}) == 1
    composed_rows = [
        row
        for row in rows
        if row["attributes"]["container"]
        == "py/rules.py:build/composed_rules"
    ]
    assert len(composed_rows) == 1
    assert composed_rows[0]["attributes"]["state"] == "unknown"
    assert composed_rows[0]["attributes"]["fragment"].startswith(
        "py/rules.py:build/composed_rules#"
    )
    composed_projection = json.loads(
        evaluate_projection_json(
            project.map_json(),
            {
                "container": "py/rules.py:build/composed_rules",
                "id": "composed-rules",
                "select": "ordered_rows",
            },
        )
    )
    assert composed_projection["completeness"]["classification"] == "incomplete"
    assert len(composed_projection["fact"]["items"]) == 1
    assert composed_projection["fact"]["items"][0]["state"] == "unknown"

    definition = {
        "container": "c/rules.c:first/rules",
        "id": "first-rules",
        "select": "ordered_rows",
    }
    projection = json.loads(evaluate_projection_json(project.map_json(), definition))
    assert projection["fact"]["shape"] == "values"
    assert projection["completeness"]["classification"] == "complete"
    assert projection["completeness"]["exhaustive"] is True
    assert sorted(
        ordinal
        for row in projection["fact"]["items"]
        for ordinal in row["value"]["ordinals"]
    ) == [0, 1]
    duplicate_projection = json.loads(
        evaluate_projection_json(
            project.map_json(),
            {
                "container": "py/rules.py:build/function_rules",
                "id": "duplicate-rules",
                "select": "ordered_rows",
            },
        )
    )
    assert len(duplicate_projection["fact"]["items"]) == 1
    assert duplicate_projection["fact"]["items"][0]["value"]["count"] == 2
    assert duplicate_projection["fact"]["items"][0]["value"]["ordinals"] == [
        0,
        1,
    ]

    missing_row = json.loads(project.map_json())
    missing_row["facts"] = [
        row
        for row in missing_row["facts"]
        if not (
            row["domain"] == "ordered-rows"
            and row["attributes"].get("container")
            == "c/rules.c:first/rules"
            and row["attributes"].get("ordinal") == 1
        )
    ]
    incomplete = json.loads(
        evaluate_projection_json(
            json.dumps(missing_row, separators=(",", ":")).encode(), definition
        )
    )
    assert incomplete["completeness"]["classification"] == "incomplete"
    assert incomplete["completeness"]["exhaustive"] is False

    gapped = json.loads(project.map_json())
    next(
        row
        for row in gapped["facts"]
        if row["domain"] == "ordered-rows"
        and row["attributes"].get("container") == "c/rules.c:first/rules"
        and row["attributes"].get("ordinal") == 1
    )["attributes"]["ordinal"] = 2
    gapped_projection = json.loads(
        evaluate_projection_json(
            json.dumps(gapped, separators=(",", ":")).encode(), definition
        )
    )
    assert gapped_projection["completeness"]["classification"] == "incomplete"

    recovered = json.loads(project.map_json())
    next(
        row
        for row in recovered["facts"]
        if row["domain"] == "ordered-containers"
        and row["attributes"].get("container") == "c/rules.c:first/rules"
    )["attributes"]["syntax_recovery"] = "intersects"
    recovered_projection = json.loads(
        evaluate_projection_json(
            json.dumps(recovered, separators=(",", ":")).encode(), definition
        )
    )
    assert recovered_projection["completeness"]["classification"] == "incomplete"

    dynamic = json.loads(
        evaluate_projection_json(
            project.map_json(),
            {
                "container": "py/rules.py:build/dynamic_rules",
                "id": "dynamic-rules",
                "select": "ordered_rows",
            },
        )
    )
    assert dynamic["completeness"]["classification"] == "incomplete"
    assert dynamic["completeness"]["exhaustive"] is False
    mutated = json.loads(
        evaluate_projection_json(
            project.map_json(),
            {
                "container": "py/rules.py:build/mutated_rules",
                "id": "mutated-rules",
                "select": "ordered_rows",
            },
        )
    )
    assert mutated["completeness"]["classification"] == "incomplete"
    assert mutated["completeness"]["exhaustive"] is False
    c_dynamic = json.loads(
        evaluate_projection_json(
            project.map_json(),
            {
                "container": "c/rules.c:expanded/rules",
                "id": "expanded-rules",
                "select": "ordered_rows",
            },
        )
    )
    assert c_dynamic["completeness"]["classification"] == "incomplete"
    assert c_dynamic["completeness"]["exhaustive"] is False

    module_projection = json.loads(
        evaluate_projection_json(
            project.map_json(),
            {
                "container": "py/rules.py:module_rules",
                "id": "module-rules",
                "select": "ordered_rows",
            },
        )
    )
    source_keys = [
        row["key"]
        for row in sorted(
            module_projection["fact"]["items"],
            key=lambda item: min(item["value"]["ordinals"]),
        )
    ]
    target_keys = [
        row["key"]
        for row in sorted(
            projection["fact"]["items"],
            key=lambda item: min(item["value"]["ordinals"]),
        )
    ]
    correspondence_config = {
        **configuration,
        "projections": {
            "module-rules": {
                "container": "py/rules.py:module_rules",
                "select": "ordered_rows",
            },
            "first-rules": {
                "container": "c/rules.c:first/rules",
                "select": "ordered_rows",
            },
        },
        "constraints": {
            "RULE-CORRESPONDENCE": {
                "actual": {"projection": "first-rules"},
                "assert": "correspondence_complete",
                "expected": {"projection": "module-rules"},
                "mapping": {
                    source_keys[0]: [target_keys[1]],
                    source_keys[1]: [target_keys[0], target_keys[1]],
                },
                "owner": "compiler",
                "rationale": "Every source row has reviewed target rows.",
                "require_target_coverage": True,
            }
        },
    }
    correspondence = json.loads(
        evaluate_constraints_json(
            json.dumps(correspondence_config, separators=(",", ":")).encode(),
            project.map_json(),
        )
    )
    assert correspondence["constraints"][0]["status"] == "pass"
    partial_known = json.loads(json.dumps(correspondence_config))
    partial_known["projections"]["module-rules"]["container"] = (
        "py/rules.py:build/composed_rules"
    )
    partial_known["constraints"]["RULE-CORRESPONDENCE"]["mapping"] = {
        composed_projection["fact"]["items"][0]["key"]: [target_keys[0]]
    }
    partial_known["constraints"]["RULE-CORRESPONDENCE"][
        "require_target_coverage"
    ] = False
    partial_known_result = json.loads(
        evaluate_constraints_json(
            json.dumps(partial_known, separators=(",", ":")).encode(),
            project.map_json(),
        )
    )["constraints"][0]
    assert partial_known_result["status"] == "unknown"
    assert set(partial_known_result["coverage"]) == {
        composed_projection["fact"]["items"][0]["key"],
        target_keys[0],
    }

    mapped_diff_request = {
        "after": {
            "container": "c/rules.c:first/rules",
            "id": "first-rules",
            "select": "ordered_rows",
        },
        "before": {
            "container": "py/rules.py:module_rules",
            "id": "module-rules",
            "select": "ordered_rows",
        },
        "mapping": {
            source_keys[0]: [target_keys[1]],
            source_keys[1]: [target_keys[0], target_keys[1]],
        },
    }
    mapped_diff = json.loads(
        _native.diff(
            project.map_json(),
            project.map_json(),
            json.dumps(mapped_diff_request, separators=(",", ":")).encode(),
        )
    )
    assert mapped_diff["artifact"] == "diff"
    assert mapped_diff["schema_version"] == 8
    assert mapped_diff["join"] == "correspondence"
    assert mapped_diff["comparison"]["state"] == "complete"
    assert mapped_diff["comparison"]["counts"] == {
        "after_unmapped": 0,
        "before_unmapped": 0,
        "mapping_after_missing": 0,
        "mapping_before_missing": 0,
        "matched": 3,
    }
    assert [group["state"] for group in mapped_diff["comparison"]["groups"]] == [
        "complete",
        "complete",
    ]

    incomplete_diff = json.loads(
        _native.diff(
            project.map_json(),
            json.dumps(missing_row, separators=(",", ":")).encode(),
            json.dumps(mapped_diff_request, separators=(",", ":")).encode(),
        )
    )
    assert incomplete_diff["comparison"]["state"] == "unknown"
    assert incomplete_diff["comparison"]["counts"] == {
        "after_unmapped": 0,
        "before_unmapped": 0,
        "mapping_after_missing": 0,
        "mapping_before_missing": 0,
        "matched": 1,
    }
    assert [
        group["state"] for group in incomplete_diff["comparison"]["groups"]
    ] == ["partial", "partial"]
    assert sorted(
        target["state"]
        for group in incomplete_diff["comparison"]["groups"]
        for target in group["targets"]
    ) == ["matched", "unresolved", "unresolved"]

    malformed_map = json.loads(project.map_json())
    malformed_map["packages"] = "invalid"
    try:
        _native.diff(
            json.dumps(malformed_map, separators=(",", ":")).encode(),
            project.map_json(),
            json.dumps(mapped_diff_request, separators=(",", ":")).encode(),
        )
    except RuntimeError as error:
        assert "packages" in str(error)
    else:
        raise AssertionError("mapped Diff accepted a malformed Map section")

    reordered_map = json.loads(project.map_json())
    reordered_rows = [
        row
        for row in reordered_map["facts"]
        if row["domain"] == "ordered-rows"
        and row["attributes"].get("container") == "c/rules.c:first/rules"
    ]
    reordered_rows[0]["attributes"]["ordinal"] = 1
    reordered_rows[1]["attributes"]["ordinal"] = 0
    source_sha256 = next(
        row["sha256"]
        for row in json.loads(project.map_json())["inputs"]
        if row["path"] == "c/rules.c"
    )
    sequence_lock_config = {
        **configuration,
        "projections": {
            "first-rules": {
                "container": "c/rules.c:first/rules",
                "select": "ordered_rows",
            }
        },
        "constraints": {
            "FIRST-RULES-LOCK": {
                "actual": {"projection": "first-rules"},
                "assert": "values_equal",
                "expected": {
                    "map": "reference",
                    "projection": "first-rules",
                    "source_lock": {"c/rules.c": source_sha256},
                },
                "owner": "compiler",
                "rationale": "The reviewed rule sequence remains unchanged.",
            }
        },
    }
    reordered = json.loads(
        evaluate_constraints_json(
            json.dumps(sequence_lock_config, separators=(",", ":")).encode(),
            json.dumps(reordered_map, separators=(",", ":")).encode(),
            maps={"reference": {"map": json.loads(project.map_json())}},
        )
    )
    assert reordered["constraints"][0]["status"] == "fail"

    missing_source = json.loads(json.dumps(correspondence_config))
    del missing_source["constraints"]["RULE-CORRESPONDENCE"]["mapping"][
        source_keys[0]
    ]
    missing_source_result = json.loads(
        evaluate_constraints_json(
            json.dumps(missing_source, separators=(",", ":")).encode(),
            project.map_json(),
        )
    )
    assert missing_source_result["constraints"][0]["status"] == "fail"

    missing_target = json.loads(json.dumps(correspondence_config))
    missing_target["constraints"]["RULE-CORRESPONDENCE"]["mapping"][
        source_keys[0]
    ] = ["f" * 64]
    missing_target_result = json.loads(
        evaluate_constraints_json(
            json.dumps(missing_target, separators=(",", ":")).encode(),
            project.map_json(),
        )
    )
    assert missing_target_result["constraints"][0]["status"] == "fail"

    incomplete_correspondence = json.loads(json.dumps(correspondence_config))
    incomplete_correspondence["projections"]["module-rules"]["container"] = (
        "py/rules.py:build/dynamic_rules"
    )
    incomplete_correspondence["constraints"]["RULE-CORRESPONDENCE"][
        "mapping"
    ] = {}
    incomplete_result = json.loads(
        evaluate_constraints_json(
            json.dumps(incomplete_correspondence, separators=(",", ":")).encode(),
            project.map_json(),
        )
    )
    assert incomplete_result["constraints"][0]["status"] == "unknown"

    duplicate_target = json.loads(json.dumps(correspondence_config))
    duplicate_target["constraints"]["RULE-CORRESPONDENCE"]["mapping"][
        source_keys[0]
    ] = [target_keys[0], target_keys[0]]
    try:
        evaluate_constraints_json(
            json.dumps(duplicate_target, separators=(",", ":")).encode(),
            project.map_json(),
        )
    except RuntimeError as error:
        assert "correspondence mapping targets must be unique" in str(error)
    else:
        raise AssertionError("duplicate correspondence targets were accepted")
    shutil.rmtree(root)


def main() -> None:
    _assert_project_configuration_conformance()
    _assert_metrics_projection()
    _assert_import_resolution_soundness()
    _assert_projection_markdown_explainability()
    _assert_partial_configuration_overlay()
    _assert_symbol_occurrence_projection()
    _assert_ordered_container_facts()
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

    typed_endpoint_map = json.loads(report_map)
    typed_endpoint_map["packages"][0].update(
        {
            "entrypoints": {
                "configured:0": "py/sample/__init__.py",
                "script:sample": "sample.api:main",
            },
            "kind": "python",
            "identity": "sample-distribution",
            "name": "python",
        }
    )
    typed_endpoint_map["edges"].append(
        {
            "kind": "package-local",
            "names": ["sample.api"],
            "source": "py/sample/__init__.py",
            "target": "package:python",
        }
    )
    typed_graph = json.loads(
        evaluate_projection_json(
            json.dumps(
                typed_endpoint_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            graph_definition,
        )
    )
    typed_nodes = {
        row["attributes"]["id"]: row
        for row in typed_graph["fact"]["items"]
        if row["attributes"]["record_kind"] == "node"
    }
    assert "package:python" in typed_nodes
    assert typed_nodes["package:python"]["label"] == "sample-distribution"
    assert "package:package:python" not in typed_nodes
    assert "external:sample.api:main" not in typed_nodes
    assert any(
        row["attributes"].get("relation_kind") == "script:sample"
        and row["attributes"].get("source") == "package:python"
        and row["attributes"].get("target") == "file:py/sample/api.py"
        for row in typed_graph["fact"]["items"]
    )

    ambiguous_entrypoint_map = json.loads(json.dumps(typed_endpoint_map))
    duplicate_module = next(
        row
        for row in ambiguous_entrypoint_map["files"]
        if row["path"] == "py/sample/api.py"
    ).copy()
    duplicate_module["path"] = "alternate/sample/api.py"
    ambiguous_entrypoint_map["files"].append(duplicate_module)
    ambiguous_entrypoints = json.loads(
        evaluate_projection_json(
            json.dumps(
                ambiguous_entrypoint_map,
                sort_keys=True,
                separators=(",", ":"),
            ).encode(),
            {"id": "ambiguous-entrypoint", "select": "package_entrypoints"},
        )
    )
    ambiguous_script = next(
        row
        for row in ambiguous_entrypoints["fact"]["items"]
        if row["attributes"]["kind"] == "script:sample"
    )
    assert ambiguous_script["state"] == "unknown"
    assert ambiguous_script["attributes"]["resolution"] == "ambiguous"
    assert "target_path" not in ambiguous_script["attributes"]
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
    assert subject_document["schema_version"] == 15
    assert reference_document["schema_version"] == 15
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
    missing_member = json.loads(json.dumps(reference_document))
    missing_member["facts"] = [
        row
        for row in missing_member["facts"]
        if not (
            row["domain"] == "constant-memberships"
            and row["kind"] == "class-collection-member"
            and row["name"] == "ADD"
        )
    ]
    incomplete_membership = json.loads(
        evaluate_projection_json(
            json.dumps(missing_member, separators=(",", ":")).encode(),
            {
                "container": "GroupOp.Required",
                "id": "missing-member",
                "select": "constant_memberships",
            },
        )
    )
    assert incomplete_membership["completeness"]["classification"] == "incomplete"
    assert incomplete_membership["completeness"]["exhaustive"] is False
    assert incomplete_membership["completeness"]["counts"]["unsupported"] == 1
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
