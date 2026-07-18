#!/usr/bin/env python3
"""Native-only regressions for facts that must never become guesses."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def check_package_export_lookup_index(repository: Path) -> None:
    source = (repository / "src/map/map_packages.c").read_text(encoding="utf-8")
    start = source.index("static int entry_defines_export")
    end = source.index("\nstatic ArchbirdStatus ensure_export_origin", start)
    implementation = source[start:end]
    if "ab_project_merged_fact_range" not in implementation:
        raise AssertionError("package export lookup bypasses the fact range index")
    if "ab_project_merged_fact_count" in implementation:
        raise AssertionError("package export lookup scans the complete fact inventory")


def check_c_test_function_candidates(extension) -> None:
    source = b"""\
static void *test_allocate(void *opaque, unsigned long size) {
  (void)opaque;
  (void)size;
  return 0;
}

static void test_direct_with_context(void *context) {
  (void)context;
}

static void test_direct_no_arguments(void) {}

static void test_unregistered_helper(void *context) {
  (void)context;
}

static void test_unregistered_no_arguments(void) {}

int main(void) {
  void *context = 0;
  void *allocation = test_allocate(context, 1);
  (void)allocation;
  test_direct_with_context(context);
  test_direct_no_arguments();
  return 0;
}
"""
    path = "test/test_function_candidates.c"
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(source),
                "language": "c",
                "layer": "c-tests",
                "path": path,
                "roles": ["source", "test"],
                "sha256": hashlib.sha256(source).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "4" * 64,
            "name": "c-test-function-candidate-fixture",
            "version": "1",
        },
        "project": "c-test-function-candidates",
        "schema_version": 1,
    }
    config = {
        "schema_version": 1,
        "project": "c-test-function-candidates",
        "layers": [
            {
                "name": "c-tests",
                "language": "c",
                "globs": [path],
            }
        ],
        "tests": [
            {
                "name": "c-functions",
                "language": "c",
                "globs": [path],
                "route_to": ["c-tests"],
            }
        ],
    }
    project = extension.project_create(canonical(manifest))
    extension.project_add_source(project, path, source)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_scan_builtin_provider(project, "lexical:c", "primary")
    extension.project_scan_builtin_provider(
        project, "syntax:tree-sitter:c", "augment"
    )
    extension.project_finalize_providers(project)
    mapped = json.loads(extension.project_map(project))
    test = mapped["tests"][0]
    if [case["selector"] for case in test["cases"]] != [
        "test_allocate",
        "test_direct_no_arguments",
        "test_direct_with_context",
    ]:
        raise AssertionError(test)
    if {
        case["selector"]
        for case in test["cases"]
        if case["selector"].startswith("test_unregistered")
    }:
        raise AssertionError(test)


def check_external_call_namespace(extension) -> None:
    source = b"def run():\n    return vendor_fetch()\n"
    path = "py/client.py"
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(source),
                "language": "python",
                "layer": "python",
                "path": path,
                "roles": ["source"],
                "sha256": hashlib.sha256(source).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "5" * 64,
            "name": "external-call-namespace-fixture",
            "version": "1",
        },
        "project": "external-call-namespace",
        "schema_version": 1,
    }
    config = {
        "schema_version": 1,
        "project": "external-call-namespace",
        "layers": [
            {
                "name": "python",
                "language": "python",
                "globs": ["py/**/*.py"],
                "external_call_namespaces": [
                    {"prefix": "vendor_", "package": "vendor-sdk"}
                ],
            }
        ],
    }
    project = extension.project_create(canonical(manifest))
    extension.project_add_source(project, path, source)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_scan_builtin_provider(project, "lexical:python", "primary")
    extension.project_finalize_providers(project)
    mapped = json.loads(extension.project_map(project))
    resolution = next(
        row for row in mapped["call_resolutions"] if row["name"] == "vendor_fetch"
    )
    if resolution["kind"] != "external" or resolution["candidates"] != [
        "package:vendor-sdk"
    ]:
        raise AssertionError(resolution)
    if not any(
        edge["kind"] == "external-call"
        and edge["source"] == path
        and edge["target"] == "package:vendor-sdk"
        and edge["names"] == ["vendor_fetch"]
        for edge in mapped["edges"]
    ):
        raise AssertionError(mapped["edges"])


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_map_correctness.py EXTENSION REPOSITORY FIXTURE"
        )
    extension = load_module("archbird._native", Path(sys.argv[1]).resolve())
    repository = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    check_package_export_lookup_index(repository)
    provider = load_module(
        "archbird_map_correctness_python_ast",
        repository / "py/archbird/providers/python_ast.py",
    )
    config = json.loads((fixture / "archbird.json").read_text(encoding="utf-8"))
    paths = sorted(
        [
            *list((fixture / "py").rglob("*.py")),
            *list((fixture / "csrc").rglob("*.c")),
            *list((fixture / "csrc").rglob("*.h")),
            *list((fixture / "csrc").rglob("*.py")),
            *list((fixture / "test").rglob("*.c")),
            *list((fixture / "test").rglob("*.h")),
            *list((fixture / "unrelated").rglob("*.h")),
        ],
        key=lambda path: path.relative_to(fixture).as_posix(),
    )
    sources = []
    for path in paths:
        relative = path.relative_to(fixture).as_posix()
        raw = path.read_bytes()
        sources.append((relative, raw))
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(raw),
                "language": "python" if relative.endswith(".py") else "c",
                "layer": (
                    "python"
                    if relative.endswith(".py")
                    else (
                        "c-core"
                        if relative.startswith("csrc/")
                        else (
                            "c-unrelated"
                            if relative.startswith("unrelated/")
                            else "c-tests"
                        )
                    )
                ),
                "path": relative,
                "roles": ["source", *(["test"] if relative.startswith("test/") else [])],
                "sha256": hashlib.sha256(raw).hexdigest(),
            }
            for relative, raw in sources
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "map-correctness-host",
            "version": "1",
        },
        "project": "map-correctness",
        "schema_version": 1,
    }
    project = extension.project_create(canonical(manifest))
    for relative, raw in sources:
        extension.project_add_source(project, relative, raw)
    extension.project_finalize_sources(project)
    extension.project_set_config(project, canonical(config))
    extension.project_scan_builtin_provider(project, "lexical:c", "primary")
    extension.project_scan_builtin_provider(
        project, "syntax:tree-sitter:c", "augment"
    )
    # Compose the portable lexical Python evidence with the precise CPython
    # provider below.  A lexical re-export token must not become a second
    # package origin beside the AST provider's exact origin.
    extension.project_scan_builtin_provider(project, "lexical:python", "augment")
    extension.project_scan_builtin_provider(
        project, "syntax:tree-sitter:python", "augment"
    )
    for relative, raw in sources:
        if not relative.endswith(".py"):
            continue
        facts = provider.python_ast_provider_facts(
            project="map-correctness",
            path=relative,
            text=raw.decode("utf-8"),
            source_manifest_sha256=extension.project_manifest_sha256(project),
        )
        document = json.loads(facts)
        bindings = {
            row["name"]: row.get("attributes", {}).get("binding")
            for row in document["facts"]
            if row["domain"] == "calls"
        }
        expected = {
            "py/caller.py": {"bool": "builtin"},
            "py/default_scope.py": {"dependency": "project"},
            "py/imported.py": {"helper": "imported"},
            "py/local.py": {"helper": "local"},
            "py/scope_stability.py": {
                "len": "builtin",
                "project_helper": "project",
                "str": "builtin",
            },
            "py/shadow.py": {"bool": "project"},
            "py/test_builtin.py": {"bool": "builtin"},
            "py/test_member.py": {"Receiver": "imported", "object": "builtin"},
            "py/test_other.py": {"other": "imported"},
            "py/widget.py": {"cls": "local"},
        }.get(relative, {})
        if bindings != expected:
            raise AssertionError(f"{relative}: bindings={bindings!r}, wanted={expected!r}")
        if relative == "py/default_scope.py":
            enclosing = sorted(
                row["attributes"]["enclosing"]
                for row in document["facts"]
                if row["domain"] == "calls" and row["name"] == "dependency"
            )
            if enclosing != ["Box", "fetch"]:
                raise AssertionError(
                    f"default/base lexical owners={enclosing!r}, wanted ['Box', 'fetch']"
                )
        extension.project_add_provider(project, "primary", facts)
    manifest_sha256 = extension.project_manifest_sha256(project)
    scope_source = dict(sources)["py/scope_stability.py"]
    scope_call_start = scope_source.index(
        b"project_helper", scope_source.index(b"def scope_00")
    )
    scope_target_start = scope_source.index(b"project_helper")
    extension.project_add_provider(
        project,
        "augment",
        canonical(
            {
                "artifact": "archbird-provider-facts",
                "capabilities": [
                    {
                        "boundary": "one deliberately stale semantic target range",
                        "claims": ["semantic-target"],
                        "coverage": "bounded",
                        "domain": "reference-targets",
                    }
                ],
                "diagnostics": [],
                "facts": [
                    {
                        "attributes": {
                            "evidence_state": "current",
                            "index": "semantic-fixture",
                            "target_path": "py/scope_stability.py",
                            "target_span_end": scope_target_start
                            + len("project_helper"),
                            "target_span_start": scope_target_start,
                            "target_symbol": "project_helper",
                        },
                        "claim": "semantic-target",
                        "domain": "reference-targets",
                        "id": "scope-00-project-helper-target",
                        "key": "scope-00-project-helper-target",
                        "kind": "reference",
                        "name": "project_helper",
                        "path": "py/scope_stability.py",
                        "project": "map-correctness",
                        "span": {
                            "end": scope_call_start + len("project_helper"),
                            "start": scope_call_start,
                        },
                    },
                    {
                        "attributes": {
                            "evidence_state": "unknown",
                            "index": "stale-fixture",
                            "target_path": "csrc/cross_file_target.h",
                            "target_span_end": 999999,
                            "target_span_start": 999998,
                            "target_symbol": "cross_file_only",
                        },
                        "claim": "semantic-target",
                        "domain": "reference-targets",
                        "id": "unknown-target-range",
                        "key": "unknown-target-range",
                        "kind": "reference",
                        "name": "unavailable_target",
                        "path": "test/system_extensionless_include.c",
                        "project": "map-correctness",
                        "span": {"end": 1, "start": 0},
                    }
                ],
                "inputs": [
                    {
                        "project": "map-correctness",
                        "source_manifest_sha256": manifest_sha256,
                    }
                ],
                "producer": {
                    "configuration_sha256": "2" * 64,
                    "implementation_sha256": "3" * 64,
                    "name": "map-correctness-stale-target",
                    "version": "1",
                },
                "provenance": "derived",
                "resolutions": [
                    {
                        "fact_id": "scope-00-project-helper-target",
                        "state": "unique",
                        "targets": ["semantic:project-helper"],
                    },
                    {
                        "fact_id": "unknown-target-range",
                        "state": "unique",
                        "targets": ["semantic:cross-file-target"],
                    }
                ],
                "schema_version": 1,
                "subject": {"project": "map-correctness", "scope": "project"},
            }
        ),
    )
    extension.project_finalize_providers(project)
    map_input_sha256 = extension.project_map_input_sha256(project)
    evidence = [
        {
            "path": "test/test_cases.c",
            "role": "runner",
            "sha256": hashlib.sha256(dict(sources)["test/test_cases.c"]).hexdigest(),
        },
        {
            "path": "csrc/callbacks.c",
            "role": "subject",
            "sha256": hashlib.sha256(dict(sources)["csrc/callbacks.c"]).hexdigest(),
        },
        {
            "path": "test/test_cases.c",
            "role": "test_inventory",
            "sha256": hashlib.sha256(dict(sources)["test/test_cases.c"]).hexdigest(),
        },
    ]
    observation = {
        "artifact": "archbird-test-symbol-observations",
        "cases": [
            {
                "group": "c",
                "path": "test/test_cases.c",
                "selector": "sched.2d_e2e",
                "symbols": [
                    {
                        "hits": 3,
                        "path": "csrc/callbacks.c",
                        "symbol": "alpha_callback",
                    }
                ],
            }
        ],
        "producer": {
            "configuration_sha256": "4" * 64,
            "implementation_sha256": "3" * 64,
            "name": "map-correctness-symbol-runner",
            "runtime": "fixture-runtime",
            "version": "1",
        },
        "project": "map-correctness",
        "provenance": "observed",
        "schema_version": 1,
        "source": {
            "config_sha256": extension.project_config_sha256(project),
            "evidence": evidence,
            "evidence_slice_sha256": hashlib.sha256(canonical(evidence)).hexdigest(),
            "map_input_sha256": map_input_sha256,
        },
    }
    stale_observation = copy.deepcopy(observation)
    stale_observation["source"]["map_input_sha256"] = "0" * 64
    try:
        extension.project_add_test_symbol_observations(
            project, canonical(stale_observation)
        )
    except Exception as error:
        if "does not match current Map input" not in str(error):
            raise
    else:
        raise AssertionError("stale observed test evidence was accepted")
    bad_slice_observation = copy.deepcopy(observation)
    bad_slice_observation["source"]["evidence_slice_sha256"] = "0" * 64
    try:
        extension.project_add_test_symbol_observations(
            project, canonical(bad_slice_observation)
        )
    except Exception as error:
        if "evidence-slice digest is invalid" not in str(error):
            raise
    else:
        raise AssertionError("invalid observed evidence-slice digest was accepted")
    extension.project_add_test_symbol_observations(project, canonical(observation))
    extension.project_add_test_symbol_observations(project, canonical(observation))
    unmapped_observation = copy.deepcopy(observation)
    unmapped_observation["producer"]["configuration_sha256"] = "5" * 64
    unmapped_observation["cases"] = [
        {
            "group": "c",
            "path": "test/test_cases.c",
            "selector": "missing.case",
            "symbols": [
                {
                    "hits": 1,
                    "path": "csrc/callbacks.c",
                    "symbol": "alpha_callback",
                }
            ],
        },
        {
            "group": "c",
            "path": "test/test_cases.c",
            "selector": "sched.2d_e2e",
            "symbols": [
                {
                    "hits": 1,
                    "path": "csrc/callbacks.c",
                    "symbol": "missing_callback",
                }
            ],
        },
    ]
    extension.project_add_test_symbol_observations(
        project, canonical(unmapped_observation)
    )
    first_map = extension.project_map(project)
    for _ in range(8):
        if extension.project_map(project) != first_map:
            raise AssertionError("repeated Map output is not byte-identical")
    mapped = json.loads(first_map)
    packages = {row["name"]: row for row in mapped["packages"]}
    star_package = packages["python-star-public"]
    if (
        star_package["export_origins"]["StarGadget"]
        != ["py/starpublic/impl.py"]
        or "StarGadget" not in star_package["exports"]
    ):
        raise AssertionError(star_package)
    components = {row["name"]: row for row in mapped["components"]}
    all_python_files = sorted(
        relative for relative, _raw in sources if relative.startswith("py/")
    )
    top_level_python_files = sorted(
        relative
        for relative in all_python_files
        if relative.count("/") == 1
    )
    if components["python-recursive"]["files"] != all_python_files:
        raise AssertionError(components["python-recursive"])
    if components["python-top-level"]["files"] != top_level_python_files:
        raise AssertionError(components["python-top-level"])
    if "py/chainpkg/nested/module.py" in components["python-top-level"]["files"]:
        raise AssertionError("single-star component selector crossed a directory")
    diagnostic_codes = {row["code"] for row in mapped["diagnostics"]}
    if not {
        "observed-symbol-unmapped",
        "observed-test-case-unmapped",
    }.issubset(diagnostic_codes):
        raise AssertionError(mapped["diagnostics"])
    scope_call = next(
        row
        for row in mapped["symbol_calls"]
        if row["source"]
        == {"path": "py/scope_stability.py", "symbol": "scope_00"}
        and row["name"] == "project_helper"
    )
    if scope_call["resolution"] != "unique" or scope_call["candidates"] != [
        {"line": 1, "path": "py/scope_stability.py", "symbol": "project_helper"}
    ]:
        raise AssertionError(scope_call)
    if [row["provider"] for row in scope_call["evidence"]] != [
        "archbird-python-ast",
        "map-correctness-stale-target",
    ]:
        raise AssertionError(scope_call)
    builtin_call = next(
        row
        for row in mapped["symbol_calls"]
        if row["source"]
        == {"path": "py/scope_stability.py", "symbol": "scope_00"}
        and row["name"] == "str"
    )
    if builtin_call["resolution"] != "builtin" or builtin_call["candidates"]:
        raise AssertionError(builtin_call)
    if len(builtin_call["evidence"]) != 2 or len(
        {
            (row["span"]["start"], row["span"]["end"])
            for row in builtin_call["evidence"]
        }
    ) != 2:
        raise AssertionError(builtin_call)
    callback_references = [
        row
        for row in mapped["symbol_references"]
        if row["source"]
        == {"path": "csrc/callbacks.c", "symbol": "callback_registry_value"}
    ]
    callback_projection = {
        (
            row["name"],
            row["context"],
            row.get("container", ""),
            row["resolution"],
        ): [(candidate["path"], candidate["symbol"]) for candidate in row["candidates"]]
        for row in callback_references
    }
    expected_callback_projection = {
        (
            "alpha_callback",
            "initializer",
            "selected",
            "candidate",
        ): [("csrc/callbacks.c", "alpha_callback")],
        (
            "alpha_callback",
            "initializer",
            "routes",
            "candidate",
        ): [("csrc/callbacks.c", "alpha_callback")],
        (
            "beta_callback",
            "assignment",
            "selected",
            "candidate",
        ): [("csrc/callbacks.c", "beta_callback")],
        (
            "beta_callback",
            "initializer",
            "routes",
            "candidate",
        ): [("csrc/callbacks.c", "beta_callback")],
        (
            "selected",
            "argument",
            "invoke_callback",
            "ambiguous",
        ): [
            ("csrc/callbacks.c", "alpha_callback"),
            ("csrc/callbacks.c", "beta_callback"),
        ],
    }
    if callback_projection != expected_callback_projection:
        raise AssertionError(callback_projection)
    if any(row["resolution"] == "unique" for row in callback_references):
        raise AssertionError(callback_references)
    observed_case = next(
        case
        for test in mapped["tests"]
        if test["group"] == "c" and test["path"] == "test/test_cases.c"
        for case in test["cases"]
        if case["selector"] == "sched.2d_e2e"
    )
    observed_rows = [
        row
        for row in observed_case["route_evidence"]
        if row["provenance"] == "observed"
    ]
    if len(observed_rows) != 1:
        raise AssertionError(observed_rows)
    observed_row = observed_rows[0]
    if {
        key: observed_row[key]
        for key in (
            "claim",
            "hits",
            "provider",
            "relation",
            "scope",
            "target",
            "target_symbol",
        )
    } != {
        "claim": "symbol-hit",
        "hits": 3,
        "provider": "map-correctness-symbol-runner",
        "relation": "observed-symbol-hit",
        "scope": "case",
        "target": "csrc/callbacks.c",
        "target_symbol": "alpha_callback",
    }:
        raise AssertionError(observed_row)
    if observed_case["routes"] != {"py/helper.py": 1}:
        raise AssertionError(
            "observations must not inflate static route counts: "
            f"{observed_case['routes']!r}"
        )
    callback_target_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["csrc/callbacks.c:alpha_callback"],
                    "direction": "upstream",
                    "depth": 0,
                    "test_depth": 0,
                }
            ),
        )
    )
    if {
        (row["context"], row.get("container", ""), row["resolution"])
        for row in callback_target_query["symbol_references"]
    } != {
        ("argument", "invoke_callback", "ambiguous"),
        ("initializer", "routes", "candidate"),
        ("initializer", "selected", "candidate"),
    }:
        raise AssertionError(callback_target_query["symbol_references"])
    container_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/widget.py:Receiver"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 0,
                }
            ),
        )
    )
    if [
        row["name"] for row in container_query["files"][0]["symbols"]
    ] != [
        "Receiver",
        "Receiver.create",
        "Receiver.chain",
        "Receiver.run",
    ]:
        raise AssertionError(container_query["files"])
    if [row["name"] for row in container_query["matched_symbols"]] != [
        "Receiver"
    ]:
        raise AssertionError(container_query["matched_symbols"])
    observed_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["csrc/callbacks.c:alpha_callback"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 0,
                }
            ),
        )
    )
    observed_match = next(
        row
        for row in observed_query["test_matches"]
        if row["path"] == "test/test_cases.c"
        and row["selector"] == "sched.2d_e2e"
    )
    if observed_match["classification"] != "observed" or observed_match[
        "evidence"
    ] != "observed":
        raise AssertionError(observed_match)
    if {
        field: observed_match[field]
        for field in ("provenance", "confidence", "seed_distance", "target_role")
    } != {
        "provenance": "observed",
        "confidence": "exact",
        "seed_distance": 0,
        "target_role": "requested-symbol",
    } or observed_match["target"] != {
        "path": "csrc/callbacks.c",
        "symbol": "alpha_callback",
    }:
        raise AssertionError(observed_match)
    if len(observed_match["route_evidence"]) != 1 or observed_match[
        "route_evidence"
    ][0]["observation_sha256"] != observed_row["observation_sha256"]:
        raise AssertionError(observed_match)
    observed_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["csrc/callbacks.c:alpha_callback"],
                "direction": "both",
                "depth": 0,
                "test_depth": 0,
            }
        ),
    ).decode()
    if "test-matches observed=1" not in observed_report or (
        "sched.2d_e2e [observed; source=observed]" not in observed_report
    ):
        raise AssertionError(observed_report)
    callback_target_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["csrc/callbacks.c:alpha_callback"],
                "direction": "upstream",
                "depth": 0,
                "test_depth": 0,
            }
        ),
    ).decode()
    for expected_callback_report in (
        "symbol-references unique=0 candidate=2 ambiguous=1 unresolved=0",
        "--value:selected [ambiguous; context=argument; container=invoke_callback",
        "--value:alpha_callback [candidate; context=initializer; container=routes",
        "--value:alpha_callback [candidate; context=initializer; container=selected",
    ):
        if expected_callback_report not in callback_target_report:
            raise AssertionError(callback_target_report)
    symbol_local_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/scope_stability.py:scope_00"],
                    "direction": "both",
                    "depth": 1,
                    "test_depth": 1,
                }
            ),
        )
    )
    if symbol_local_query["query"]["scope"] != "symbol" or symbol_local_query[
        "query"
    ]["symbol_evidence_state"] != "current":
        raise AssertionError(symbol_local_query["query"])
    if [
        (row["path"], [item["name"] for item in row["symbols"]])
        for row in symbol_local_query["files"]
    ] != [("py/scope_stability.py", ["scope_00", "project_helper"])]:
        raise AssertionError(symbol_local_query["files"])
    if {
        (row["name"], row["resolution"])
        for row in symbol_local_query["symbol_calls"]
    } != {("project_helper", "unique"), ("str", "builtin")}:
        raise AssertionError(symbol_local_query["symbol_calls"])
    symbol_local_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["py/scope_stability.py:scope_00"],
                "direction": "both",
                "depth": 1,
                "test_depth": 1,
            }
        ),
    ).decode()
    for expected_report_evidence in (
        "symbol-calls unique=1 candidate=0 ambiguous=0 unresolved=0 method=0 "
        "builtin=1 external=0",
        "scope_00 --str [builtin; evidence=2",
        "scope_00 --project_helper [unique; evidence=2",
        "map-correctness-stale-target@byte+",
    ):
        if expected_report_evidence not in symbol_local_report:
            raise AssertionError(symbol_local_report)
    resolutions = {
        (row["source"], row["name"]): row for row in mapped["call_resolutions"]
    }
    if resolutions[("py/caller.py", "bool")]["kind"] != "builtin":
        raise AssertionError(resolutions[("py/caller.py", "bool")])
    if resolutions[("py/local.py", "helper")]["kind"] != "unresolved":
        raise AssertionError(resolutions[("py/local.py", "helper")])
    if resolutions[("py/imported.py", "helper")]["kind"] != "candidate":
        raise AssertionError(resolutions[("py/imported.py", "helper")])
    if resolutions[("py/shadow.py", "bool")]["kind"] != "unique":
        raise AssertionError(resolutions[("py/shadow.py", "bool")])
    cross_file = resolutions[("test/cross_file_candidate.c", "cross_file_only")]
    if cross_file["kind"] != "candidate" or cross_file["candidates"] != [
        "csrc/cross_file_target.c"
    ]:
        raise AssertionError(cross_file)
    cross_file_impact = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["csrc/cross_file_target.c:cross_file_only"],
                    "direction": "upstream",
                    "depth": 1,
                    "test_depth": 1,
                }
            ),
        )
    )
    if [
        (row["path"], [symbol["name"] for symbol in row["symbols"]])
        for row in cross_file_impact["files"]
    ] != [
        ("csrc/cross_file_target.c", ["cross_file_only"]),
        (
            "test/cross_file_candidate.c",
            ["cross_file_caller_one", "cross_file_caller_two"],
        ),
    ]:
        raise AssertionError(cross_file_impact["files"])
    if {
        (row["source"]["symbol"], row["resolution"])
        for row in cross_file_impact["symbol_calls"]
        if "symbol" in row["source"]
    } != {
        ("cross_file_caller_one", "candidate"),
        ("cross_file_caller_two", "candidate"),
    } or any(
        not row["evidence"]
        or any(
            evidence["span"]["start"] >= evidence["span"]["end"]
            for evidence in row["evidence"]
        )
        for row in cross_file_impact["symbol_calls"]
    ):
        raise AssertionError(cross_file_impact["symbol_calls"])
    cross_file_match = next(
        row
        for row in cross_file_impact["test_matches"]
        if row["path"] == "test/cross_file_candidate.c"
        and row["selector"] == "cross.candidate"
    )
    if (
        cross_file_match["classification"] != "candidate"
        or cross_file_match["provenance"] != "derived"
        or cross_file_match["confidence"] != "candidate"
        or cross_file_match["seed_distance"] != 0
        or cross_file_match["target_role"] != "requested-symbol"
        or cross_file_match["target"]
        != {
            "path": "csrc/cross_file_target.c",
            "symbol": "cross_file_only",
        }
    ):
        raise AssertionError(cross_file_match)
    cross_file_seed_only = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["csrc/cross_file_target.c:cross_file_only"],
                    "direction": "upstream",
                    "depth": 0,
                    "test_depth": 0,
                }
            ),
        )
    )
    if [row["path"] for row in cross_file_seed_only["files"]] != [
        "csrc/cross_file_target.c"
    ]:
        raise AssertionError(cross_file_seed_only["files"])
    for name, kind in {
        "len": "builtin",
        "project_helper": "unique",
        "str": "builtin",
    }.items():
        resolution = resolutions[("py/scope_stability.py", name)]
        if resolution["kind"] != kind:
            raise AssertionError(resolution)
    edges = {
        (row["kind"], row["source"], row["target"], name)
        for row in mapped["edges"]
        for name in row["names"]
    }
    if (
        "imported-call",
        "py/imported.py",
        "py/helper.py",
        "helper",
    ) not in edges:
        raise AssertionError(f"missing imported helper edge: {sorted(edges)!r}")
    if any(source == "py/caller.py" for _kind, source, _target, _name in edges):
        raise AssertionError(f"builtin call leaked a project edge: {sorted(edges)!r}")
    if any(source == "py/local.py" for _kind, source, _target, _name in edges):
        raise AssertionError(f"local callback leaked a project edge: {sorted(edges)!r}")
    if any(
        source == "test/cross_file_candidate.c" and name == "cross_file_only"
        for _kind, source, _target, name in edges
    ):
        raise AssertionError(f"candidate call leaked a proven edge: {sorted(edges)!r}")
    if (
        "import",
        "test/cross_file_candidate.c",
        "csrc/cross_file_target.h",
        "cross_file_target.h",
    ) not in edges:
        raise AssertionError(f"missing configured C include edge: {sorted(edges)!r}")
    if (
        "import",
        "test/local_include.c",
        "test/local_only.h",
        "local_only.h",
    ) not in edges:
        raise AssertionError(f"missing local C include edge: {sorted(edges)!r}")
    for expected in {
        ("import", "test/dual_include.c", "test/dual.h", "dual.h"),
        ("import", "test/dual_include.c", "csrc/dual.h", "dual.h"),
    }:
        if expected not in edges:
            raise AssertionError(f"missing delimiter-specific edge: {sorted(edges)!r}")
    if any(
        source
        in {
            "test/absolute_include.c",
            "test/system_extensionless_include.c",
            "test/system_include.c",
            "test/windows_absolute_include.c",
        }
        and target
        in {
            "csrc/cross_file_target.h",
            "csrc/version.py",
            "test/local_only.h",
        }
        for _kind, source, target, _name in edges
    ):
        raise AssertionError(f"unsafe C include became an edge: {sorted(edges)!r}")
    if (
        "import",
        "test/cross_file_candidate.c",
        "unrelated/cross_file_target.h",
        "cross_file_target.h",
    ) in edges:
        raise AssertionError(f"foreign layer root became an edge: {sorted(edges)!r}")
    if any(
        source == "test/system_extensionless_include.c"
        and name == "unavailable_target"
        for _kind, source, _target, name in edges
    ):
        raise AssertionError(f"stale semantic target became an edge: {sorted(edges)!r}")
    if not any(
        row["code"] == "provider-target-span-unavailable"
        and row["path"] == "csrc/cross_file_target.h"
        and row["severity"] == "warning"
        for row in mapped["diagnostics"]
    ):
        raise AssertionError(f"missing stale target diagnostic: {mapped['diagnostics']!r}")
    typed_edges = {
        (row["kind"], row["source"], row["target"], name)
        for row in mapped["edges"]
        for name in row["names"]
    }
    if (
        "member-call",
        "py/test_member.py",
        "py/widget.py",
        "Widget.build",
    ) not in typed_edges:
        raise AssertionError(f"missing imported-member edge: {sorted(typed_edges)!r}")
    if any(
        source == "py/test_member.py" and name == "Receiver.run"
        for _kind, source, _target, name in typed_edges
    ):
        raise AssertionError(
            f"receiver candidate leaked a proven edge: {sorted(typed_edges)!r}"
        )
    if (
        "decorator",
        "py/test_member.py",
        "py/widget.py",
        "Wrapper",
    ) not in typed_edges:
        raise AssertionError(f"missing decorator edge: {sorted(typed_edges)!r}")
    if (
        "member-call",
        "py/test_public_member.py",
        "py/publicpkg/impl.py",
        "Gadget.make",
    ) not in typed_edges:
        raise AssertionError(f"missing reexported-member edge: {sorted(typed_edges)!r}")
    if (
        "member-call",
        "py/test_public_attribute.py",
        "py/publicpkg/impl.py",
        "Gadget.make",
    ) not in typed_edges:
        raise AssertionError(
            f"missing package-root attribute edge: {sorted(typed_edges)!r}"
        )
    cases = [
        case
        for test in mapped["tests"]
        if test["path"] == "test/test_cases.c"
        for case in test["cases"]
    ]
    if [case["selector"] for case in cases] != ["sched.2d_e2e"]:
        raise AssertionError(cases)
    if cases[0]["configured_routes"] != ["py/helper.py"]:
        raise AssertionError(cases[0])

    candidate_case = next(
        case
        for test in mapped["tests"]
        if test["path"] == "test/cross_file_candidate.c"
        for case in test["cases"]
        if case["selector"] == "cross.candidate"
    )
    if candidate_case["routes"] != {"csrc/cross_file_target.c": 1}:
        raise AssertionError(candidate_case)
    if len(candidate_case["route_evidence"]) != 1 or candidate_case[
        "route_evidence"
    ][0]["relation"] != "call-candidate":
        raise AssertionError(candidate_case)

    tests = {test["path"]: test for test in mapped["tests"]}
    generated = tests["test/generated.c"]
    if not generated["generated"]:
        raise AssertionError(generated)
    if generated["generated_from"] != ["test/test_cases.c"]:
        raise AssertionError(generated)
    if generated["count_unit"] != "static_case_occurrence":
        raise AssertionError(generated)
    for path in ("test/dispatch.c", "py/dispatch.py"):
        dispatch = tests[path]
        if [case["selector"] for case in dispatch["cases"]] != [
            "alpha",
            "beta_2d",
        ]:
            raise AssertionError(dispatch)
        if {case["evidence_kind"] for case in dispatch["cases"]} != {
            "named_dispatch_entry"
        }:
            raise AssertionError(dispatch)
        if dispatch["count"] != 2:
            raise AssertionError(dispatch)

    import_case = tests["py/test_import_use.py"]["cases"][0]
    if import_case["routes"] != {"py/helper.py": 1}:
        raise AssertionError(import_case)
    if import_case["route_evidence"] != [
        {
            "claim": "syntax-structure",
            "enclosing": "test_import_use",
            "fact_id": import_case["route_evidence"][0]["fact_id"],
            "line": 5,
            "name": "helper",
            "provenance": "derived",
            "provider": "archbird-python-ast",
            "relation": "imported-name-reference",
            "scope": "case",
            "span": {"end": 81, "start": 75},
            "target": "py/helper.py",
            "target_symbol": "helper",
        }
    ]:
        raise AssertionError(import_case["route_evidence"])
    chain_case = tests["py/test_chain.py"]["cases"][0]
    exact_chain = [
        row
        for row in chain_case["route_evidence"]
        if row["target"] == "py/chainpkg/nested/module.py"
    ]
    if len(exact_chain) != 1 or {
        key: exact_chain[0][key]
        for key in ("provenance", "relation", "scope", "target_symbol")
    } != {
        "provenance": "derived",
        "relation": "imported-attribute-call",
        "scope": "case",
        "target_symbol": "target",
    }:
        raise AssertionError(chain_case)
    candidate_case = tests["py/test_chain_candidate.py"]["cases"][0]
    uncertain_chain = [
        row
        for row in candidate_case["route_evidence"]
        if row["target"] == "py/candidatepkg/hidden.py"
    ]
    if len(uncertain_chain) != 1 or uncertain_chain[0]["relation"] != (
        "imported-attribute-candidate"
    ):
        raise AssertionError(candidate_case)
    member_case = next(
        case
        for case in tests["py/test_member.py"]["cases"]
        if case["selector"] == "test_imported_member"
    )
    member_evidence = [
        row
        for row in member_case["route_evidence"]
        if row["target_symbol"] == "Widget.build"
    ]
    if len(member_evidence) != 1 or member_evidence[0]["relation"] != (
        "imported-member-call"
    ):
        raise AssertionError(member_case)
    for selector in ("test_constructed_receiver", "test_chained_receiver"):
        receiver_case = next(
            case
            for case in tests["py/test_member.py"]["cases"]
            if case["selector"] == selector
        )
        receiver_evidence = [
            row
            for row in receiver_case["route_evidence"]
            if row["target_symbol"] == "Receiver.run"
        ]
        if len(receiver_evidence) != 1 or receiver_evidence[0]["relation"] != (
            "inferred-receiver-candidate"
        ):
            raise AssertionError(receiver_case)
    reassigned_case = next(
        case
        for case in tests["py/test_member.py"]["cases"]
        if case["selector"] == "test_reassigned_receiver"
    )
    if any(
        row["target_symbol"] == "Receiver.run"
        for row in reassigned_case["route_evidence"]
    ):
        raise AssertionError(reassigned_case)
    public_member_case = tests["py/test_public_member.py"]["cases"][0]
    public_member_evidence = [
        row
        for row in public_member_case["route_evidence"]
        if row["target_symbol"] == "Gadget.make"
    ]
    if len(public_member_evidence) != 1 or public_member_evidence[0][
        "relation"
    ] != "imported-member-call":
        raise AssertionError(public_member_case)
    alias_member_case = next(
        case
        for case in tests["py/test_public_member.py"]["cases"]
        if case["selector"] == "test_aliased_member"
    )
    alias_member_evidence = [
        row
        for row in alias_member_case["route_evidence"]
        if row["target_symbol"] == "Gadget.make"
    ]
    if len(alias_member_evidence) != 1 or alias_member_evidence[0][
        "relation"
    ] != "imported-member-call":
        raise AssertionError(alias_member_case)
    public_attribute_case = tests["py/test_public_attribute.py"]["cases"][0]
    public_attribute_evidence = [
        row
        for row in public_attribute_case["route_evidence"]
        if row["target_symbol"] == "Gadget.make"
    ]
    if len(public_attribute_evidence) != 1 or public_attribute_evidence[0][
        "relation"
    ] != "imported-member-call":
        raise AssertionError(public_attribute_case)
    decorator_case = next(
        case
        for case in tests["py/test_member.py"]["cases"]
        if case["selector"] == "test_decorated_callable"
    )
    decorator_relations = {
        (row["relation"], row["target_symbol"])
        for row in decorator_case["route_evidence"]
    }
    if (
        "decorator-reference",
        "Wrapper",
    ) not in decorator_relations or (
        "decorator-callable-candidate",
        "Wrapper.__call__",
    ) not in decorator_relations:
        raise AssertionError(decorator_case)
    configured_evidence = [
        row
        for row in cases[0]["route_evidence"]
        if row["provenance"] == "asserted"
    ]
    if len(configured_evidence) != 1 or {
        key: configured_evidence[0][key]
        for key in ("provenance", "provider", "relation", "scope", "target")
    } != {
        "provenance": "asserted",
        "provider": "project-config",
        "relation": "configured",
        "scope": "case",
        "target": "py/helper.py",
    }:
        raise AssertionError(configured_evidence)

    helper_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["helper"],
                    "direction": "both",
                    "depth": 1,
                    "test_depth": 3,
                }
            ),
        )
    )
    matches = {
        (match["path"], match["selector"]): match
        for match in helper_query["test_matches"]
    }
    direct = matches[("py/test_import_use.py", "test_import_use")]
    if (
        direct["classification"] != "direct"
        or direct["provenance"] != "derived"
        or direct["confidence"] != "exact"
        or direct["evidence_scope"] != "case"
        or direct["seed_distance"] != 0
        or direct["target_role"] != "requested-symbol"
        or direct["target"] != {"path": "py/helper.py", "symbol": "helper"}
        or {row["relation"] for row in direct["route_evidence"]}
        != {"imported-name-reference"}
    ):
        raise AssertionError(direct)
    asserted = matches[("test/test_cases.c", "sched.2d_e2e")]
    if (
        asserted["classification"] != "asserted"
        or asserted["provenance"] != "asserted"
        or asserted["confidence"] != "exact"
        or asserted["evidence_scope"] != "case"
        or asserted["seed_distance"] != 0
        or asserted["target_role"] != "requested-symbol"
        or asserted["target"] != {"path": "py/helper.py", "symbol": "helper"}
        or {row["provenance"] for row in asserted["route_evidence"]}
        != {"asserted"}
    ):
        raise AssertionError(asserted)
    conservative = matches[("py/test_other.py", "test_other")]
    if (
        conservative["classification"] != "conservative"
        or conservative["provenance"] != "derived"
        or conservative["confidence"] != "conservative"
        or conservative["evidence_scope"] != "file"
        or conservative["seed_distance"] != 0
        or conservative["target_role"] != "requested-symbol"
        or conservative["target"] != {"path": "py/helper.py"}
        or conservative["evidence"] != "static"
        or conservative["route_evidence"]
    ):
        raise AssertionError(conservative)
    helper_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["helper"],
                "direction": "both",
                "depth": 1,
                "test_depth": 3,
            }
        ),
    ).decode()
    if (
        "test-matches observed=0 direct=1 asserted=1 candidate=0 conservative=1 "
        "unresolved=0"
        not in helper_report
    ):
        raise AssertionError(helper_report)
    if "[direct; source=static]" not in helper_report:
        raise AssertionError(helper_report)
    if "py/test_other.py:4:test_other [conservative; source=static]" in (
        helper_report
    ) or "1 conservative routes collapsed" not in helper_report:
        raise AssertionError(helper_report)
    if "## Selection manifest" not in helper_report:
        raise AssertionError(helper_report)
    budget_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["helper"],
                "direction": "both",
                "depth": 1,
                "test_depth": 3,
            }
        ),
        1200,
    ).decode()
    for budget_evidence in (
        "Emitted: files=0/3 canonical=3; symbol-calls=0/2; "
        "symbol-references=0/0; test-matches=0/2.",
        "Budget=1200 characters; sections=0/2; ranked-file node cap=0; "
        "canonical-files=3.",
        "Omitted complete sections: Ranked neighborhood Routed evidence",
    ):
        if budget_evidence not in budget_report:
            raise AssertionError(budget_report)
    if "--context-offset symbol_calls=0" in budget_report:
        raise AssertionError(budget_report)
    expanded_helper_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["helper"],
                "direction": "both",
                "depth": 1,
                "test_depth": 3,
                "context": {"conservative": "expand"},
            }
        ),
    ).decode()
    if (
        "py/test_other.py:4:test_other [conservative; source=static]"
        not in expanded_helper_report
    ):
        raise AssertionError(expanded_helper_report)
    quota_context = {
        "profile": "change",
        "quotas": {"test_matches": 1},
    }
    quota_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["helper"],
                    "direction": "both",
                    "depth": 1,
                    "test_depth": 3,
                    "context": quota_context,
                }
            ),
        )
    )
    if quota_query["test_matches"] != helper_query["test_matches"]:
        raise AssertionError("context policy filtered canonical Query JSON")
    quota_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["helper"],
                "direction": "both",
                "depth": 1,
                "test_depth": 3,
                "context": quota_context,
            }
        ),
    ).decode()
    if (
        "route-selection emitted=1 eligible=2 candidate-collapsed=0 "
        "conservative-collapsed=1 excluded=0 offset=0"
        not in quota_report
        or "--context-offset test_matches=1" not in quota_report
        or "sched.2d_e2e [asserted; source=configured]" not in quota_report
        or "test_import_use [direct; source=static]" in quota_report
    ):
        raise AssertionError(quota_report)
    continued_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["helper"],
                "direction": "both",
                "depth": 1,
                "test_depth": 3,
                "context": {
                    "profile": "change",
                    "quotas": {"test_matches": 1},
                    "offsets": {"test_matches": 1},
                },
            }
        ),
    ).decode()
    if (
        "route-selection emitted=1 eligible=2 candidate-collapsed=0 "
        "conservative-collapsed=1 excluded=0 offset=1"
        not in continued_report
        or "test_import_use [direct; source=static]" not in continued_report
        or "sched.2d_e2e [asserted; source=configured]" in continued_report
    ):
        raise AssertionError(continued_report)
    exact_report = extension.map_query_markdown(
        canonical(mapped),
        canonical(
            {
                "symbols": ["helper"],
                "direction": "both",
                "depth": 1,
                "test_depth": 3,
                "context": {"profile": "exact"},
            }
        ),
    ).decode()
    if (
        "Context: profile=exact; max-seed-distance=0; candidate=exclude; "
        "conservative=exclude; files=1/1."
        not in exact_report
        or "py/imported.py layer=python" in exact_report
        or "py/local.py layer=python" in exact_report
    ):
        raise AssertionError(exact_report)
    try:
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["helper"],
                    "context": {"profile": "invented"},
                }
            ),
        )
    except Exception:
        pass
    else:
        raise AssertionError("invalid context profile was accepted")
    try:
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["helper"],
                    "context": {"candidate": "invented"},
                }
            ),
        )
    except Exception:
        pass
    else:
        raise AssertionError("invalid candidate policy was accepted")
    for invalid_count_kind in (
        "files",
        "symbol_calls",
        "symbol_references",
        "test_matches",
    ):
        try:
            extension.map_query(
                canonical(mapped),
                canonical(
                    {
                        "symbols": ["helper"],
                        "context": {"quotas": {invalid_count_kind: "bad"}},
                    }
                ),
            )
        except Exception:
            pass
        else:
            raise AssertionError(
                f"invalid context quota was accepted for {invalid_count_kind}"
            )

    related_route_map = copy.deepcopy(mapped)
    related_test = next(
        test
        for test in related_route_map["tests"]
        if test["path"] == "py/test_other.py"
    )
    related_case = related_test["cases"][0]
    related_case["configured_routes"] = []
    related_case["routes"] = {"py/imported.py": 1}
    related_case["route_evidence"] = [
        {
            "provenance": "derived",
            "provider": "map-correctness-fixture",
            "relation": "imported-call",
            "scope": "case",
            "target": "py/imported.py",
            "target_symbol": "imported_call",
        }
    ]
    related_case["selector"] = "test_related_symbol_route"
    related_query = json.loads(
        extension.map_query(
            canonical(related_route_map),
            canonical(
                {
                    "symbols": ["py/helper.py:helper"],
                    "direction": "upstream",
                    "depth": 1,
                    "test_depth": 1,
                }
            ),
        )
    )
    related_match = next(
        match
        for match in related_query["test_matches"]
        if match["selector"] == "test_related_symbol_route"
    )
    if {
        field: related_match[field]
        for field in (
            "classification",
            "provenance",
            "confidence",
            "seed_distance",
            "target_role",
            "target",
        )
    } != {
        "classification": "direct",
        "provenance": "derived",
        "confidence": "exact",
        "seed_distance": 1,
        "target_role": "symbol-neighborhood",
        "target": {"path": "py/imported.py", "symbol": "imported_call"},
    }:
        raise AssertionError(related_match)

    method_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["Box.bool"],
                    "direction": "both",
                    "depth": 1,
                    "test_depth": 3,
                }
            ),
        )
    )
    if any(
        match["path"] == "py/test_builtin.py"
        for match in method_query["test_matches"]
    ):
        raise AssertionError(method_query["test_matches"])

    if {
        row["path"] for row in method_query["matched_symbols"]
    } != {"py/alternate_methods.py", "py/methods.py"}:
        raise AssertionError(method_query["matched_symbols"])
    qualified_method_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/methods.py:Box.bool"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 0,
                }
            ),
        )
    )
    if [
        (row["path"], row["name"])
        for row in qualified_method_query["matched_symbols"]
    ] != [("py/methods.py", "Box.bool")]:
        raise AssertionError(qualified_method_query["matched_symbols"])
    if qualified_method_query["query"]["focus"] != [
        "symbol:py/methods.py:Box.bool"
    ]:
        raise AssertionError(qualified_method_query["query"])

    exact_chain_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/chainpkg/nested/module.py:target"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    exact_chain_matches = {
        (row["path"], row["selector"]): row
        for row in exact_chain_query["test_matches"]
    }
    if exact_chain_matches[("py/test_chain.py", "test_imported_module_chain")][
        "classification"
    ] != "direct":
        raise AssertionError(exact_chain_query["test_matches"])

    candidate_chain_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/candidatepkg/hidden.py:target"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    candidate_chain_matches = {
        (row["path"], row["selector"]): row
        for row in candidate_chain_query["test_matches"]
    }
    if candidate_chain_matches[
        ("py/test_chain_candidate.py", "test_unproven_module_chain")
    ]["classification"] != "candidate":
        raise AssertionError(candidate_chain_query["test_matches"])

    member_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/widget.py:Widget.build"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    member_matches = {
        (row["path"], row["selector"]): row
        for row in member_query["test_matches"]
    }
    if member_matches[("py/test_member.py", "test_imported_member")][
        "classification"
    ] != "direct":
        raise AssertionError(member_query["test_matches"])

    receiver_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/widget.py:Receiver.run"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    receiver_matches = {
        (row["path"], row["selector"]): row
        for row in receiver_query["test_matches"]
    }
    for selector in ("test_constructed_receiver", "test_chained_receiver"):
        match = receiver_matches[("py/test_member.py", selector)]
        if match["classification"] != "candidate" or {
            row["relation"] for row in match["route_evidence"]
        } != {"inferred-receiver-candidate"}:
            raise AssertionError(match)
    if receiver_matches[("py/test_member.py", "test_reassigned_receiver")][
        "classification"
    ] != "conservative":
        raise AssertionError(receiver_query["test_matches"])

    semantic_leaf_map = copy.deepcopy(mapped)
    semantic_member_case = next(
        case
        for test in semantic_leaf_map["tests"]
        if test["path"] == "py/test_member.py"
        for case in test["cases"]
        if case["selector"] == "test_imported_member"
    )
    semantic_member_evidence = next(
        row
        for row in semantic_member_case["route_evidence"]
        if row["target_symbol"] == "Widget.build"
    )
    semantic_member_evidence["target_symbol"] = "build"
    semantic_leaf_query = json.loads(
        extension.map_query(
            canonical(semantic_leaf_map),
            canonical(
                {
                    "symbols": ["py/widget.py:Widget.build"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    semantic_leaf_match = next(
        row
        for row in semantic_leaf_query["test_matches"]
        if row["selector"] == "test_imported_member"
    )
    if semantic_leaf_match["classification"] != "direct":
        raise AssertionError(semantic_leaf_match)

    ambiguous_leaf_map = copy.deepcopy(semantic_leaf_map)
    widget_file = next(
        row for row in ambiguous_leaf_map["files"] if row["path"] == "py/widget.py"
    )
    widget_symbol = next(
        row for row in widget_file["symbols"] if row["name"] == "Widget.build"
    )
    other_symbol = copy.deepcopy(widget_symbol)
    other_symbol["name"] = "Other.build"
    widget_file["symbols"].append(other_symbol)
    ambiguous_leaf_query = json.loads(
        extension.map_query(
            canonical(ambiguous_leaf_map),
            canonical(
                {
                    "symbols": ["py/widget.py:Widget.build"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    ambiguous_leaf_match = next(
        row
        for row in ambiguous_leaf_query["test_matches"]
        if row["selector"] == "test_imported_member"
    )
    if ambiguous_leaf_match["classification"] != "conservative":
        raise AssertionError(ambiguous_leaf_match)

    callable_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/widget.py:Wrapper.__call__"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 1,
                }
            ),
        )
    )
    callable_matches = {
        (row["path"], row["selector"]): row
        for row in callable_query["test_matches"]
    }
    candidate = callable_matches[
        ("py/test_member.py", "test_decorated_callable")
    ]
    if candidate["classification"] != "candidate" or {
        row["relation"] for row in candidate["route_evidence"]
    } != {"decorator-callable-candidate"}:
        raise AssertionError(candidate)

    other_query = json.loads(
        extension.map_query(
            canonical(mapped),
            canonical(
                {
                    "symbols": ["py/helper.py:other"],
                    "direction": "both",
                    "depth": 0,
                    "test_depth": 3,
                }
            ),
        )
    )
    c_fallbacks = [
        row
        for row in other_query["test_matches"]
        if row["path"] == "test/test_cases.c"
    ]
    if not c_fallbacks or any(
        row["classification"] != "conservative" for row in c_fallbacks
    ):
        raise AssertionError(other_query["test_matches"])

    report = extension.map_markdown(canonical(mapped)).decode()
    for required in (
        "they do not claim runtime collection or execution",
        "source_static_occurrences=4",
        "generated_static_occurrences=1",
        "unique_source_case_selectors=4",
        "named_dispatch_entries=2",
        "csrc/cross_file_target.c(1)",
        "py/helper.py(1)",
    ):
        if required not in report:
            raise AssertionError(f"missing honest test-report label: {required!r}")

    suite = {
        "schema_version": 1,
        "suite": "configured-route-provenance",
        "projects": {"subject": {"map": "subject.map.json"}},
        "extractors": {
            "routes": {
                "kind": "test_routes",
                "project": "subject",
                "group": "c",
                "configured_only": True,
            }
        },
        "checks": [
            {
                "id": "CONFIGURED-ROUTE",
                "assert": "cardinality",
                "actual": "routes",
                "min": 1,
                "owner": "test",
                "rationale": "Configured routes retain distinct case and assertion witnesses.",
            },
            {
                "id": "MISSING-ROUTE",
                "assert": "min_test_routes",
                "actual": "routes",
                "min": 1,
                "required_routes": ["py/missing.py"],
                "owner": "test",
                "rationale": "Act must preserve route evidence provenance.",
            }
        ],
    }
    verify_input = {
        "schema_version": 1,
        "artifact": "verification-input",
        "suite_path": "configured-route.verify.json",
        "projects": [
            {
                "name": "subject",
                "map": mapped,
                "sources": [
                    {"path": relative, "text": raw.decode("utf-8")}
                    for relative, raw in sources
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
    route = verified["facts"][0]["items"][0]
    if route["attributes"]["kind"] != "configured":
        raise AssertionError(route)
    if {row["provenance"] for row in route["evidence"]} != {
        "asserted",
        "derived",
    }:
        raise AssertionError(route["evidence"])
    evidence_by_provenance = {
        row["provenance"]: row["detail"] for row in route["evidence"]
    }
    if " -> " in evidence_by_provenance["derived"] or not (
        evidence_by_provenance["derived"].endswith(
            "selected by configured route"
        )
        and evidence_by_provenance["asserted"].startswith(
            "configured structural test route"
        )
    ):
        raise AssertionError(route["evidence"])
    missing_check = next(
        row for row in verified["checks"] if row["id"] == "MISSING-ROUTE"
    )
    if missing_check["status"] != "fail":
        raise AssertionError(missing_check)
    proposal = json.loads(
        extension.change_proposal(
            canonical(verified), missing_check["findings"][0]["fingerprint"]
        )
    )
    proposal_evidence = [
        evidence
        for candidate in proposal["candidates"]
        for evidence in candidate["evidence"]
    ] + [
        evidence
        for postcondition in proposal["postconditions"]
        for evidence in postcondition["evidence"]
    ]
    if not any(
        row["provenance"] == "asserted"
        and row["detail"].startswith("configured structural test route")
        for row in proposal_evidence
    ) or any(
        row["provenance"] == "derived"
        and "(configured)" in row["detail"]
        for row in proposal_evidence
    ):
        raise AssertionError(proposal_evidence)
    check_c_test_function_candidates(extension)
    check_external_call_namespace(extension)
    print(
        "typed calls, preprocessing-token selectors, named dispatch, and "
        "generated test provenance passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
