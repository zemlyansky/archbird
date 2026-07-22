from __future__ import annotations

import json
from pathlib import Path

from archbird.errors import ConfigError
from archbird.native import (
    Project,
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

    query_options, query_plan, projection_results = compile_named_query(
        config_json=json.dumps(
            modern, sort_keys=True, separators=(",", ":")
        ).encode(),
        query_id="api-impact",
        map_json=modern_map,
    )
    assert query_plan["id"] == "api-impact"
    assert len(query_plan["projection_definitions"]) == 2
    assert len(projection_results) == 2
    assert "projection_result_sha256" not in query_plan["projection_definitions"][0]
    assert query_plan["projection_definitions"][0][
        "projection_definition_sha256"
    ] == projection_results[0]["projection_definition_sha256"]
    assert query_options["symbols"]
    public_query_plan = json.loads(
        compile_query_plan_json(
            json.dumps(modern, sort_keys=True, separators=(",", ":")).encode(),
            modern_map,
            "api-impact",
        )
    )
    assert public_query_plan == {
        "artifact": "query-plan",
        "plan": query_plan,
        "projection_results": projection_results,
        "request": query_options,
        "schema_version": 1,
    }
    named_query = json.loads(
        query_map_json(
            modern_map,
            **query_options,
            plan=query_plan,
            projection_results=projection_results,
        )
    )
    assert named_query["query"]["saved_plan"] == query_plan
    assert named_query["query"]["projection_results"] == projection_results

    inferred_options, inferred_plan, inferred_results = compile_named_query(
        config_json=json.dumps(
            modern, sort_keys=True, separators=(",", ":")
        ).encode(),
        query_id="inferred-impact",
        map_json=modern_map,
    )
    assert len(inferred_plan["projection_definitions"]) == 14
    assert len(inferred_results) == 14
    inferred_query = json.loads(
        query_map_json(
            modern_map,
            **inferred_options,
            plan=inferred_plan,
            projection_results=inferred_results,
        )
    )
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

    ad_hoc_options, ad_hoc_plan, ad_hoc_results = compile_ad_hoc_query(
        modern_map, {"paths": ["src/base/**"]}
    )
    assert ad_hoc_plan["id"] == "ad-hoc"
    assert ad_hoc_plan["project_configuration_sha256"] is None
    assert len(ad_hoc_plan["projection_definitions"]) == 1
    assert len(ad_hoc_results) == 1
    ad_hoc_query = json.loads(
        query_map_json(
            modern_map,
            **ad_hoc_options,
            plan=ad_hoc_plan,
            projection_results=ad_hoc_results,
        )
    )
    direct_path_query = json.loads(
        query_map_json(modern_map, paths=["src/base/**"])
    )
    assert ad_hoc_query["files"] == direct_path_query["files"]

    invalid_named_plan = dict(query_plan)
    invalid_named_plan["project_configuration_sha256"] = None
    try:
        query_map_json(
            modern_map,
            **query_options,
            plan=invalid_named_plan,
            projection_results=projection_results,
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
            **ad_hoc_options,
            plan=invalid_ad_hoc_plan,
            projection_results=ad_hoc_results,
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
        assert "provider_surface requires a non-empty name" in str(error)
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
    _, metadata_plan, metadata_results = compile_named_query(
        json.dumps(metadata_only, sort_keys=True, separators=(",", ":")).encode(),
        "api-impact",
        modern_map,
    )
    assert metadata_plan["query_plan_sha256"] == query_plan["query_plan_sha256"]
    assert metadata_plan["project_configuration_sha256"] != query_plan[
        "project_configuration_sha256"
    ]
    assert metadata_results == projection_results
    mismatched_model = json.loads(json.dumps(modern))
    mismatched_model["layers"][0]["globs"].append("not-present/**/*.c")
    mismatched_json = json.dumps(
        mismatched_model, sort_keys=True, separators=(",", ":")
    ).encode()
    try:
        compile_named_query(mismatched_json, "api-impact", modern_map)
    except ConfigError as error:
        assert "Map definition does not match saved Map" in str(error)
    else:
        raise AssertionError("named Query accepted a mismatched Map definition")
    try:
        evaluate_constraints_json(mismatched_json, modern_map)
    except RuntimeError as error:
        assert "Map definition does not match current Map" in str(error)
    else:
        raise AssertionError("Verify accepted a mismatched Map definition")
    _, overridden_plan, _ = compile_named_query(
        json.dumps(modern, sort_keys=True, separators=(",", ":")).encode(),
        "api-impact",
        modern_map,
        overrides={"depth": 3},
    )
    assert overridden_plan["query_plan_sha256"] != query_plan["query_plan_sha256"]

    reordered = json.loads(json.dumps(modern))
    reordered["queries"]["api-impact"]["projection"] = [
        "core-engine",
        "public-api",
        {"paths": ["include/**"], "select": "symbols"},
    ]
    _, reordered_plan, reordered_results = compile_named_query(
        json.dumps(reordered, sort_keys=True, separators=(",", ":")).encode(),
        "api-impact",
        modern_map,
    )
    assert reordered_plan["query_plan_sha256"] == query_plan["query_plan_sha256"]
    assert reordered_results == projection_results

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
    assert proposal["source"]["policy"]["sha256"] == failing_result["policy"][
        "constraint_policy_sha256"
    ]
    assert change_proposal(
        failing_verification, fingerprint, format="markdown"
    ).startswith(b"# Architecture change proposal: MISSING-LITERAL\n")
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


if __name__ == "__main__":
    main()
