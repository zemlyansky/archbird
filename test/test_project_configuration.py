from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil

from archbird import _native
from archbird.errors import ConfigError
from archbird.native import (
    Project,
    Source,
    change_contract,
    change_proposal,
    change_verify,
    compile_project_configuration,
    compile_query_plan_json,
    diff_maps_json,
    evaluate_constraints_json,
    evaluate_projection_json,
    freeze_constraints_json,
    publish_okf_bundle,
    query_map_json,
)
from archbird.project_configuration import compile_ad_hoc_query, compile_named_query


ROOT = Path(__file__).resolve().parents[1]


def _reseal(document: dict) -> bytes:
    canonical = {
        key: value for key, value in document.items() if key != "sha256"
    }
    document["sha256"] = hashlib.sha256(
        json.dumps(
            canonical, sort_keys=True, separators=(",", ":")
        ).encode()
    ).hexdigest()
    return json.dumps(
        document, sort_keys=True, separators=(",", ":")
    ).encode()


def _seal_verification(document: dict) -> bytes:
    canonical = {
        key: value
        for key, value in document.items()
        if key != "verification_result_sha256"
    }
    document["verification_result_sha256"] = hashlib.sha256(
        json.dumps(
            canonical, sort_keys=True, separators=(",", ":")
        ).encode()
    ).hexdigest()
    return json.dumps(
        document, sort_keys=True, separators=(",", ":")
    ).encode()


def _assert_invalid_proposal(document: dict, expected: str) -> None:
    try:
        change_contract(
            _reseal(document),
            objective="Restore the reviewed literal.",
            owner="architecture",
            rationale="Reject malformed canonical change artifacts.",
        )
    except RuntimeError as error:
        assert expected in str(error)
    else:
        raise AssertionError("Act accepted a malformed resealed proposal")


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
            "schema_version": 2,
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
            "schema_version": 2,
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


def main() -> None:
    _assert_project_configuration_conformance()
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
    assert compiled["map_definition"]["schema_version"] == 2
    assert "queries" not in compiled["map_definition"]
    assert "constraints" not in compiled["map_definition"]

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

    inventory_map = json.loads(_fixture_map(ROOT / "test/fixtures/map_packages"))
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
    } == expected_component_edges

    config_json = json.dumps(
        modern, sort_keys=True, separators=(",", ":")
    ).encode()
    query_plan = compile_named_query(config_json, "api-impact")
    assert query_plan["id"] == "api-impact"
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
        "schema_version": 2,
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

    inferred_plan = compile_named_query(config_json, "inferred-impact")
    assert len(inferred_plan["projections"]) == 14
    inferred_query = json.loads(query_map_json(modern_map, plan=inferred_plan))
    assert len(inferred_query["query"]["projection_results"]) == 14
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
    assert ad_hoc_plan["project_configuration_sha256"] is None
    assert len(ad_hoc_plan["projections"]) == 1
    ad_hoc_query = json.loads(query_map_json(modern_map, plan=ad_hoc_plan))
    assert len(ad_hoc_query["query"]["projection_results"]) == 1
    direct_path_query = json.loads(
        query_map_json(modern_map, paths=["src/base/**"])
    )
    assert ad_hoc_query["files"] == direct_path_query["files"]

    same_line_config = {
        "schema_version": 2,
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
        "schema_version": 2,
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
        "schema_version": 2,
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
        "schema_version": 2,
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
    assert subject_document["schema_version"] == 9
    assert reference_document["schema_version"] == 9
    assert {row["domain"] for row in subject_document["facts"]} == {
        "constant-values",
        "macro-invocations",
    }
    assert {row["domain"] for row in reference_document["facts"]} == {
        "constant-memberships",
        "constant-values",
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
    assert make_input["roles"] == ["provider"]
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
    assert json.loads(
        change_proposal(provider_verification, provider_finding["fingerprint"])
    )["origin"]["constraint"] == "PROVIDER-RENAME"
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
    assert changed_compiled["map_config_sha256"] == compiled["map_config_sha256"]
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
    assert mismatched_plan["map_config_sha256"] != query_plan[
        "map_config_sha256"
    ]
    try:
        query_map_json(modern_map, plan=mismatched_plan)
    except RuntimeError as error:
        assert "Map configuration does not match query input" in str(error)
    else:
        raise AssertionError("Query execution accepted a mismatched Map definition")
    try:
        evaluate_constraints_json(mismatched_json, modern_map)
    except RuntimeError as error:
        assert "Map definition does not match current Map" in str(error)
    else:
        raise AssertionError("Verify accepted a mismatched Map definition")
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
    browser_finding = browser_constraint["findings"][0]
    browser_proposal = json.loads(
        change_proposal(browser_verification, browser_finding["fingerprint"])
    )
    assert {
        (row["kind"], row["name"])
        for row in browser_proposal["evidence_slice"]["entries"]
        if row["kind"] == "observation"
    } == {("observation", "reference"), ("observation", "subject")}

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
    assert markdown.startswith(b"# Architecture constraints: archbird\n")
    assert b"constraint policy" in markdown
    assert b"## Constraints\n" in markdown
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
    proposal_bytes = change_proposal(
        failing_verification, fingerprint, pretty=False
    )
    proposal = json.loads(proposal_bytes)
    assert proposal["artifact"] == "change-proposal"
    assert proposal["schema_version"] == 2
    assert "constraint" in proposal["origin"]
    assert "constraint_sha256" in proposal["origin"]
    assert "check" not in proposal["origin"]
    assert all("constraint" in row for row in proposal["postconditions"])
    assert all(
        fact["completeness"]["classification"] == "complete"
        for fact in proposal["facts"]
    )
    assert proposal["source"]["policy"]["sha256"] == failing_result["policy"][
        "constraint_policy_sha256"
    ]
    forged_verification = json.loads(failing_verification)
    forged_verification["verification_result_sha256"] = "0" * 64
    try:
        change_proposal(
            json.dumps(
                forged_verification, sort_keys=True, separators=(",", ":")
            ).encode(),
            fingerprint,
        )
    except RuntimeError as error:
        assert "result SHA-256" in str(error)
    else:
        raise AssertionError(
            "Act accepted a verification artifact with a forged result identity"
        )
    contradictory_verification = json.loads(failing_verification)
    contradictory_verification["policy"]["project"] = "another-project"
    try:
        change_proposal(
            _seal_verification(contradictory_verification),
            fingerprint,
        )
    except RuntimeError as error:
        assert "verification artifact" in str(error)
    else:
        raise AssertionError(
            "Act accepted contradictory verification project identities"
        )
    incomplete_verification = json.loads(failing_verification)
    current_evaluation = next(
        row
        for row in incomplete_verification["evaluations"]
        if row["id"] == "current"
    )
    del current_evaluation["resolution_sha256"]
    try:
        change_proposal(
            _seal_verification(incomplete_verification),
            fingerprint,
        )
    except RuntimeError as error:
        assert "Map evaluation" in str(error)
    else:
        raise AssertionError(
            "Act accepted an incomplete verification evaluation identity"
        )
    assert change_proposal(
        failing_verification, fingerprint, format="markdown"
    ).startswith(b"# Architecture change proposal: MISSING-LITERAL\n")
    malformed_proposal = json.loads(proposal_bytes)
    malformed_proposal["projections"][0]["selection"] = 7
    _assert_invalid_proposal(malformed_proposal, "projection")
    malformed_proposal = json.loads(proposal_bytes)
    malformed_proposal["source"]["evaluation"]["resolution_sha256"] = 7
    _assert_invalid_proposal(malformed_proposal, "source")
    malformed_proposal = json.loads(proposal_bytes)
    malformed_proposal["source"]["policy"]["project"] = "another-project"
    _assert_invalid_proposal(malformed_proposal, "source")
    malformed_proposal = json.loads(proposal_bytes)
    malformed_proposal["origin"]["finding"]["message"] = 7
    _assert_invalid_proposal(malformed_proposal, "finding")
    malformed_proposal = json.loads(proposal_bytes)
    entries = malformed_proposal["evidence_slice"]["entries"]
    entries.append(entries[0])
    malformed_proposal["evidence_slice"]["sha256"] = hashlib.sha256(
        json.dumps(entries, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    _assert_invalid_proposal(malformed_proposal, "evidence slice")
    contract_bytes = change_contract(
        proposal_bytes,
        objective="Restore the reviewed literal.",
        owner="architecture",
        rationale="Exercise the state-bound constraint lifecycle.",
        pretty=False,
    )
    contract = json.loads(contract_bytes)
    assert contract["artifact"] == "change-contract"
    assert contract["schema_version"] == 2
    assert "constraint" in contract["origin"]
    assert "preserved_constraints" in contract
    assert "preserved_checks" not in contract
    assert change_contract(
        proposal_bytes,
        objective="Restore the reviewed literal.",
        owner="architecture",
        rationale="Exercise the state-bound constraint lifecycle.",
        format="markdown",
    ).startswith(b"# Reviewed architecture change contract: MISSING-LITERAL\n")
    malformed_contract = json.loads(contract_bytes)
    malformed_contract["origin"]["fingerprint"] = 7
    try:
        change_verify(
            proposal_bytes,
            _reseal(malformed_contract),
            failing_verification,
            failing_verification,
        )
    except RuntimeError as error:
        assert "contract" in str(error)
    else:
        raise AssertionError("Act accepted a malformed resealed contract")
    change_result = json.loads(
        change_verify(
            proposal_bytes,
            contract_bytes,
            failing_verification,
            failing_verification,
            pretty=False,
        )
    )
    assert change_result["artifact"] == "change-result"
    assert change_result["schema_version"] == 2
    assert change_result["status"] == "missing"
    assert any(row["status"] == "missing" for row in change_result["outcomes"])

    transition_root = ROOT / "build/act-transition-test"
    shutil.rmtree(transition_root, ignore_errors=True)
    before_root = transition_root / "before"
    after_root = transition_root / "after"
    transition_config = {
        "schema_version": 2,
        "project": "act-transition",
        "layers": [
            {
                "globs": ["src/**/*.py"],
                "language": "python",
                "name": "python",
            }
        ],
        "limits": {"max_file_bytes": 64},
        "constraints": {
            "REQUIRED-SYMBOL": {
                "kind": "required_symbols",
                "owner": "architecture",
                "rationale": "The reviewed entrypoint remains present.",
                "symbols": ["required"],
            }
        },
    }
    transition_json = json.dumps(
        transition_config, sort_keys=True, separators=(",", ":")
    ).encode()
    for repository, source in (
        (before_root, "def other():\n    return 0\n"),
        (after_root, "def required():\n    return 1\n"),
    ):
        (repository / "src").mkdir(parents=True)
        (repository / "src/module.py").write_text(source, encoding="utf-8")

    def transition_verification(repository: Path) -> bytes:
        project = Project.from_repository(
            repository, config=transition_json, jobs=1
        )
        return evaluate_constraints_json(transition_json, project.map_json())

    before_transition = transition_verification(before_root)
    after_transition = transition_verification(after_root)
    before_document = json.loads(before_transition)
    after_document = json.loads(after_transition)
    assert before_document["constraints"][0]["status"] == "fail"
    assert after_document["constraints"][0]["status"] == "pass"
    transition_fingerprint = before_document["constraints"][0]["findings"][0][
        "fingerprint"
    ]
    transition_proposal = change_proposal(
        before_transition, transition_fingerprint, pretty=False
    )
    transition_contract = change_contract(
        transition_proposal,
        objective="Restore the reviewed entrypoint.",
        owner="architecture",
        rationale="Exercise complete and incomplete transition outcomes.",
        pretty=False,
    )
    satisfied = json.loads(
        change_verify(
            transition_proposal,
            transition_contract,
            before_transition,
            after_transition,
            pretty=False,
        )
    )
    assert satisfied["status"] == "satisfied"
    assert {row["status"] for row in satisfied["outcomes"]} == {"satisfied"}
    unresolved = json.loads(
        change_verify(
            transition_proposal,
            transition_contract,
            before_transition,
            before_transition,
            pretty=False,
        )
    )
    assert unresolved["status"] == "missing"
    assert any(row["status"] == "missing" for row in unresolved["outcomes"])

    incomplete_transition_config = {
        "schema_version": 2,
        "project": "incomplete-act-transition",
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
        "constraints": {
            "NORMALIZED-CONSTANTS": {
                "actual": {"projection": "normalized"},
                "assert": "values_equal",
                "expected": {"literal": {"TWO": 3}},
                "owner": "architecture",
                "rationale": "Incomplete normalized identities cannot pass.",
            }
        },
    }
    incomplete_transition_json = json.dumps(
        incomplete_transition_config, sort_keys=True, separators=(",", ":")
    ).encode()

    def constant_verification(source: bytes) -> bytes:
        project = Project(
            "incomplete-act-transition",
            (
                Source(
                    "src/constants.c",
                    source,
                    language="c",
                    layer="c",
                ),
            ),
        )
        project.set_config(incomplete_transition_json)
        project.scan(jobs=1, map_cache=False)
        return evaluate_constraints_json(
            incomplete_transition_json, project.map_json()
        )

    complete_failure = constant_verification(b"enum Ops { API_ONE = 1 };\n")
    incomplete_after = constant_verification(
        b"enum Ops { API_ONE = 1, ONE = 2, API_TWO = 3 };\n"
    )
    complete_failure_document = json.loads(complete_failure)
    incomplete_after_document = json.loads(incomplete_after)
    assert complete_failure_document["constraints"][0]["status"] == "fail"
    assert incomplete_after_document["constraints"][0]["status"] == "unknown"
    missing_constant = next(
        row
        for row in complete_failure_document["constraints"][0]["findings"]
        if row["comparison"] == "missing"
    )
    incomplete_proposal = change_proposal(
        complete_failure, missing_constant["fingerprint"], pretty=False
    )
    incomplete_contract = change_contract(
        incomplete_proposal,
        objective="Restore the reviewed constant.",
        owner="architecture",
        rationale="Incomplete postcondition evidence remains unknown.",
        pretty=False,
    )
    incomplete_result = json.loads(
        change_verify(
            incomplete_proposal,
            incomplete_contract,
            complete_failure,
            incomplete_after,
            pretty=False,
        )
    )
    assert incomplete_result["status"] == "unknown"
    assert any(
        row["kind"] == "postcondition" and row["status"] == "unknown"
        for row in incomplete_result["outcomes"]
    )
    shutil.rmtree(transition_root)


if __name__ == "__main__":
    main()
