from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from archbird import _native
from archbird.acting import _validate_plan_shape, preview_plan
from archbird.native import Project
from archbird.planning import generate_plan


BUILD = ROOT / "build"
PRODUCER_SHA256 = hashlib.sha256(b"producer").hexdigest()
INPUT_SHA256 = hashlib.sha256(b"input").hexdigest()
CONFIG_SHA256 = hashlib.sha256(b"configuration").hexdigest()
POLICY_SHA256 = hashlib.sha256(b"policy").hexdigest()
def _digest(value: object) -> str:
    encoded = json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    return hashlib.sha256(_native.json_canonicalize(encoded)).hexdigest()


def _write(root: Path, path: str, content: bytes) -> str:
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    return hashlib.sha256(content).hexdigest()


def _file(
    path: str,
    sha256: str,
    *,
    symbols: list[dict[str, object]] | None = None,
    exports: list[str] | None = None,
) -> dict[str, object]:
    return {
        "path": path,
        "sha256": sha256,
        "symbols": symbols or [],
        "exports": exports or [],
    }


def _map(
    files: list[dict[str, object]],
    **collections: object,
) -> dict[str, object]:
    document: dict[str, object] = {
        "artifact": "map",
        "schema_version": 10,
        "project": "planning-test",
        "description": "Plan generation fixture.",
        "files": files,
        "edges": [],
        "symbol_calls": [],
        "symbol_references": [],
        "call_resolutions": [],
        "packages": [],
        "surfaces": [],
        "evidence": {
            "absolute_paths_included": False,
            "input_sha256": INPUT_SHA256,
            "config_sha256": CONFIG_SHA256,
        },
        "tool": {
            "name": "archbird",
            "version": "0.0.1",
            "implementation_sha256": PRODUCER_SHA256,
        },
    }
    document.update(collections)
    return document


def _finding(
    key: str,
    *,
    path: str = "",
    sha256: str | None = None,
    comparison: str = "extra",
    evidence_state: str = "current",
) -> dict[str, object]:
    evidence_sha256 = sha256 or hashlib.sha256(
        f"evidence:{key}".encode()
    ).hexdigest()
    return {
        "fingerprint": hashlib.sha256(f"finding:{key}:{path}".encode()).hexdigest(),
        "comparison": comparison,
        "evidence_state": evidence_state,
        "applicability": "applicable",
        "disposition": "open",
        "key": key,
        "message": f"{comparison} actual fact {key}",
        "evidence": [
            {
                "provenance": "derived" if path else "asserted",
                "project": "planning-test" if path else "",
                "path": path,
                "line": 1 if path else 0,
                "sha256": evidence_sha256,
                "detail": f"evidence for {key}",
            }
        ],
    }


def _forbidden_path_operand(
    name: str, paths: list[str]
) -> tuple[dict[str, object], dict[str, object]]:
    operand_sha256 = hashlib.sha256(
        f"operand:{name}:{','.join(paths)}".encode()
    ).hexdigest()
    items = []
    for path in paths:
        items.append(
            {
                "attributes": {},
                "evidence": [
                    {
                        "provenance": "derived",
                        "project": "planning-test",
                        "path": path,
                        "line": 0,
                        "sha256": operand_sha256,
                        "detail": f"repository inventory path {path}",
                    }
                ],
                "key": path,
                "label": path,
                "message": "",
                "state": "current",
                "value": None,
            }
        )
    operand = {
        "completeness": {
            "classification": "complete",
            "counts": {
                "evaluated": len(paths),
                "excluded": 0,
                "selected": len(paths),
                "universe": len(paths),
                "unknown": 0,
                "unsupported": 0,
            },
            "exhaustive": True,
            "truncated": False,
            "unit": "repository_file",
        },
        "items": items,
        "message": "",
        "name": name,
        "project": "planning-test",
        "provenance": "derived",
        "sha256": operand_sha256,
        "shape": "set",
        "state": "current",
    }
    finding = _finding(
        "cardinality",
        sha256=operand_sha256,
        comparison="different",
    )
    finding["message"] = (
        f"fact cardinality is {len(paths)}; expected exactly 0"
    )
    finding["evidence"][0].update(
        {
            "provenance": "derived",
            "project": "planning-test",
            "detail": f"fact {name} shape=set items={len(paths)}",
        }
    )
    return operand, finding


def _symbol_operand(
    name: str,
    values: list[str],
    evidence: dict[str, list[dict[str, object]]] | None = None,
) -> dict[str, object]:
    operand_sha256 = hashlib.sha256(
        f"operand:{name}:{','.join(values)}".encode()
    ).hexdigest()
    items = []
    for value in values:
        rows = (evidence or {}).get(value)
        if rows is None:
            rows = [
                {
                    "provenance": "asserted",
                    "project": "",
                    "path": "",
                    "line": 0,
                    "sha256": operand_sha256,
                    "detail": f"literal symbol {value}",
                }
            ]
        items.append(
            {
                "attributes": {},
                "evidence": rows,
                "key": value,
                "label": value,
                "message": "",
                "state": "current",
                "value": None,
            }
        )
    return {
        "completeness": {
            "classification": "complete",
            "counts": {
                "evaluated": len(values),
                "excluded": 0,
                "selected": len(values),
                "universe": len(values),
                "unknown": 0,
                "unsupported": 0,
            },
            "exhaustive": True,
            "truncated": False,
            "unit": "symbol",
        },
        "items": items,
        "message": "",
        "name": name,
        "project": "planning-test",
        "provenance": "derived" if evidence else "asserted",
        "sha256": operand_sha256,
        "shape": "set",
        "state": "current",
    }


def _check(
    constraint_id: str,
    *,
    assertion: str,
    actual: str,
    status: str = "fail",
    findings: list[dict[str, object]] | None = None,
    exact: int | None = None,
    expected: str = "",
) -> dict[str, object]:
    return {
        "id": constraint_id,
        "assert": assertion,
        "status": status,
        "owner": "architecture",
        "rationale": "Exercise deterministic Plan derivation.",
        "severity": "error",
        "coverage": [],
        "findings": findings or [],
        "operands": {
            "actual": actual,
            "expected": expected,
            "mapping": "",
            "exact": exact,
            "min": None,
            "max": None,
            "reference_route": "",
            "required_routes": [],
        },
        "requirements": [],
        "tags": [],
        "witnesses": [],
    }


def _verification(
    map_document: dict[str, object],
    checks: list[dict[str, object]],
    definitions: dict[str, object],
    operands: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    policy_rows = [
        {
            "id": check["id"],
            "constraint_definition_sha256": hashlib.sha256(
                f"definition:{check['id']}".encode()
            ).hexdigest(),
            "constraint_plan_sha256": hashlib.sha256(
                f"plan:{check['id']}".encode()
            ).hexdigest(),
            "constraint_result_sha256": hashlib.sha256(
                f"result:{check['id']}".encode()
            ).hexdigest(),
        }
        for check in checks
    ]
    document: dict[str, object] = {
        "artifact": "verification",
        "schema_version": 2,
        "constraints": checks,
        "diagnostics": [],
        "evaluations": [
            {
                "id": "current",
                "project": map_document["project"],
                "map_input_sha256": INPUT_SHA256,
                "map_config_sha256": CONFIG_SHA256,
                "map_producer_implementation_sha256": PRODUCER_SHA256,
                "resolution_sha256": None,
            }
        ],
        "mappings": {},
        "observations": [],
        "operand_definitions": definitions,
        "operands": operands or [],
        "policy": {
            "kind": "all",
            "project": map_document["project"],
            "configured_count": len(checks),
            "evaluated_count": len(checks),
            "omitted_count": 0,
            "requested_ids": [],
            "constraint_policy_sha256": POLICY_SHA256,
            "project_configuration_sha256": CONFIG_SHA256,
            "constraints": policy_rows,
        },
        "summary": {
            "blocking": any(check["status"] == "fail" for check in checks)
        },
        "tool": {
            "name": "archbird",
            "version": "0.0.1",
            "implementation_sha256": PRODUCER_SHA256,
        },
    }
    document["verification_result_sha256"] = _digest(document)
    return document


class PlanGenerationTest(unittest.TestCase):
    def setUp(self) -> None:
        BUILD.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="plan-generation-", dir=BUILD
        )
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def generate(
        self,
        map_document: dict[str, object],
        verification: dict[str, object],
        constraint_ids: list[str] | None = None,
    ) -> dict[str, object]:
        plan = generate_plan(
            map_document, verification, constraint_ids, self.root
        )
        _validate_plan_shape(plan)
        return plan

    def test_exact_forbidden_path_is_source_locked_delete(self) -> None:
        source_sha256 = _write(self.root, "legacy.py", b"obsolete = True\n")
        config = json.dumps(
            {
                "project": "planning-test",
                "constraints": {
                    "NO-LEGACY": {
                        "kind": "forbidden_paths",
                        "paths": ["legacy.py"],
                        "owner": "architecture",
                        "rationale": "Legacy files stay absent.",
                    },
                    "KEEP-API": {
                        "kind": "forbidden_paths",
                        "paths": ["missing.txt"],
                        "owner": "architecture",
                        "rationale": "Unrelated policy stays satisfied.",
                    },
                },
            },
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        project = Project.from_repository(
            self.root,
            config=config,
            jobs=1,
            cache_dir=None,
            map_cache=False,
        )
        map_document = json.loads(project.map_json())
        verification = json.loads(project.verify_json())

        plan = self.generate(map_document, verification, ["NO-LEGACY"])
        self.assertEqual(plan["artifact"], "plan")
        self.assertEqual(plan["provenance"], "derived")
        self.assertEqual(plan["preserved_constraints"], ["KEEP-API"])
        self.assertEqual(plan["unknowns"], [])
        self.assertEqual(plan["source"]["map"]["sha256"], _digest(map_document))
        self.assertEqual(
            plan["source"]["verification"]["sha256"],
            verification["verification_result_sha256"],
        )
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(item["non_executable_reasons"], [])
        self.assertEqual(
            item["operation"],
            {
                "action": "delete_file",
                "path": "legacy.py",
                "source_sha256": source_sha256,
            },
        )
        self.assertEqual(
            set(item["origins"][0]),
            {
                "constraint_id",
                "constraint_result_sha256",
                "issue_fingerprint",
            },
        )
        preview = preview_plan(plan, self.root)
        self.assertEqual(preview["status"], "preview")
        self.assertEqual(preview["changes"][0]["kind"], "delete")
        self.assertTrue((self.root / "legacy.py").exists())
        repeated = self.generate(map_document, verification, ["NO-LEGACY"])
        self.assertEqual(item["id"], repeated["items"][0]["id"])

    def test_native_multiple_forbidden_paths_generate_individual_deletes(
        self,
    ) -> None:
        source_hashes = {
            "legacy.py": _write(self.root, "legacy.py", b"first = 1\n"),
            "legacy_two.py": _write(
                self.root, "legacy_two.py", b"second = 2\n"
            ),
        }
        config = json.dumps(
            {
                "project": "planning-test",
                "constraints": {
                    "NO-LEGACY": {
                        "kind": "forbidden_paths",
                        "paths": ["legacy*.py"],
                        "owner": "architecture",
                        "rationale": "Legacy modules stay absent.",
                    }
                },
            },
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        project = Project.from_repository(
            self.root,
            config=config,
            jobs=1,
            cache_dir=None,
            map_cache=False,
        )
        map_document = json.loads(project.map_json())
        verification = json.loads(project.verify_json())

        check = verification["constraints"][0]
        self.assertEqual(check["findings"][0]["key"], "cardinality")
        plan = self.generate(map_document, verification)
        self.assertEqual(
            [
                item["operation"]["path"]
                for item in plan["items"]
            ],
            ["legacy.py", "legacy_two.py"],
        )
        self.assertTrue(all(item["executable"] for item in plan["items"]))
        self.assertEqual(
            {
                item["operation"]["source_sha256"]
                for item in plan["items"]
            },
            set(source_hashes.values()),
        )
        self.assertEqual(
            len(
                {
                    item["origins"][0]["issue_fingerprint"]
                    for item in plan["items"]
                }
            ),
            1,
        )
        repeated = self.generate(map_document, verification)
        self.assertEqual(
            [item["id"] for item in plan["items"]],
            [item["id"] for item in repeated["items"]],
        )

    def test_known_path_consumer_blocks_automatic_delete(self) -> None:
        legacy_sha256 = _write(self.root, "legacy.py", b"VALUE = 1\n")
        _write(
            self.root, "consumer.py", b"from legacy import VALUE\n"
        )
        config = json.dumps(
            {
                "project": "planning-test",
                "constraints": {
                    "NO-LEGACY": {
                        "kind": "forbidden_paths",
                        "paths": ["legacy.py"],
                        "owner": "architecture",
                        "rationale": "Legacy modules stay absent.",
                    }
                }
            },
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        project = Project.from_repository(
            self.root,
            config=config,
            jobs=1,
            cache_dir=None,
            map_cache=False,
        )
        map_document = json.loads(project.map_json())
        verification = json.loads(project.verify_json())

        plan = self.generate(map_document, verification)
        item = plan["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "delete_file")
        self.assertIn(
            "not current, complete",
            item["non_executable_reasons"][0],
        )
        self.assertEqual(item["unknowns"], [plan["unknowns"][0]["id"]])

    def test_build_participation_requires_manual_rewrite(self) -> None:
        legacy_sha256 = _write(self.root, "legacy.py", b"VALUE = 1\n")
        map_document = _map(
            [_file("legacy.py", legacy_sha256)],
            builds=[
                {
                    "command": "python legacy.py",
                    "conditions": [],
                    "deps": [],
                    "name": "compile",
                    "paths": ["legacy.py"],
                    "source": "Makefile",
                }
            ],
        )
        operand, finding = _forbidden_path_operand(
            "p.paths", ["legacy.py"]
        )
        check = _check(
            "NO-LEGACY",
            assertion="cardinality",
            actual="p.paths",
            exact=0,
            findings=[finding],
        )
        verification = _verification(
            map_document,
            [check],
            {"p.paths": {"select": "inventory_paths", "include": ["legacy.py"]}},
            [operand],
        )

        item = self.generate(map_document, verification)["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertEqual(item["operation"]["candidate_paths"], ["legacy.py"])
        self.assertTrue(
            any(
                "build:compile:path" in reason
                for reason in item["non_executable_reasons"]
            )
        )

    def test_test_participation_requires_manual_rewrite(self) -> None:
        legacy_sha256 = _write(self.root, "legacy.py", b"VALUE = 1\n")
        test_sha256 = _write(
            self.root, "test/test_legacy.py", b"import legacy\n"
        )
        map_document = _map(
            [
                _file("legacy.py", legacy_sha256),
                _file("test/test_legacy.py", test_sha256),
            ],
            tests=[
                {
                    "path": "test/test_legacy.py",
                    "generated_from": [],
                    "routes": {"legacy.py": 1},
                    "route_evidence": [
                        {"target": "legacy.py"}
                    ],
                    "cases": [],
                }
            ],
        )
        operand, finding = _forbidden_path_operand(
            "p.paths", ["legacy.py"]
        )
        check = _check(
            "NO-LEGACY",
            assertion="cardinality",
            actual="p.paths",
            exact=0,
            findings=[finding],
        )
        verification = _verification(
            map_document,
            [check],
            {"p.paths": {"select": "inventory_paths", "include": ["legacy.py"]}},
            [operand],
        )

        item = self.generate(map_document, verification)["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertTrue(
            any(
                "test:test/test_legacy.py:route" in reason
                for reason in item["non_executable_reasons"]
            )
        )

    def test_exact_unreferenced_forbidden_symbol_is_range_replacement(self) -> None:
        source = b"def _obsolete():\n    return 1\n\nVALUE = 2\n"
        source_sha256 = _write(self.root, "api.py", source)
        config = json.dumps(
            {
                "project": "planning-test",
                "constraints": {
                    "NO-OBSOLETE": {
                        "kind": "forbidden_symbols",
                        "symbols": ["_obsolete"],
                        "paths": ["api.py"],
                        "owner": "architecture",
                        "rationale": "Obsolete API stays absent.",
                    }
                },
            },
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        project = Project.from_repository(
            self.root,
            config=config,
            jobs=1,
            cache_dir=None,
            map_cache=False,
        )
        map_document = json.loads(project.map_json())
        verification = json.loads(project.verify_json())
        symbol = next(
            row
            for file_row in map_document["files"]
            if file_row["path"] == "api.py"
            for row in file_row["symbols"]
            if row["name"] == "_obsolete"
        )
        end = symbol["extent"]["end"]

        item = self.generate(map_document, verification)["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(
            item["operation"],
            {
                "action": "replace_range",
                "path": "api.py",
                "source_sha256": source_sha256,
                "start_byte": 0,
                "end_byte": end,
                "before": source[:end].decode(),
                "replacement": "",
            },
        )
        preview = preview_plan(
            self.generate(map_document, verification), self.root
        )
        self.assertEqual(preview["status"], "preview")
        self.assertIn(
            "-def _obsolete():", preview["changes"][0]["unified_diff"]
        )
        self.assertEqual((self.root / "api.py").read_bytes(), source)

    def test_primitive_symbol_constraints_remove_or_propose_exact_rename(
        self,
    ) -> None:
        source = b"def keep():\n    pass\n\ndef _obsolete():\n    pass\n"
        source_sha256 = _write(self.root, "api.py", source)
        config = json.dumps(
            {
                "project": "planning-test",
                "projections": {
                    "api-symbols": {
                        "select": "symbols",
                        "paths": ["api.py"],
                    }
                },
                "constraints": {
                    "SYMBOL-SUBSET": {
                        "assert": "subset",
                        "actual": {"projection": "api-symbols"},
                        "expected": {"literal": ["keep"]},
                        "owner": "architecture",
                        "rationale": "Only reviewed symbols remain.",
                    },
                    "SYMBOL-DISJOINT": {
                        "assert": "disjoint",
                        "actual": {"projection": "api-symbols"},
                        "expected": {"literal": ["_obsolete"]},
                        "owner": "architecture",
                        "rationale": "Obsolete symbols remain absent.",
                    },
                    "SYMBOL-EQUAL": {
                        "assert": "set_equal",
                        "actual": {"projection": "api-symbols"},
                        "expected": {"literal": ["keep", "missing"]},
                        "owner": "architecture",
                        "rationale": "The reviewed symbol set remains exact.",
                    },
                },
            },
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        project = Project.from_repository(
            self.root,
            config=config,
            jobs=1,
            cache_dir=None,
            map_cache=False,
        )
        map_document = json.loads(project.map_json())
        verification = json.loads(project.verify_json())

        for constraint_id in ("SYMBOL-SUBSET", "SYMBOL-DISJOINT"):
            plan = self.generate(
                map_document, verification, [constraint_id]
            )
            self.assertEqual(len(plan["items"]), 1)
            item = plan["items"][0]
            self.assertTrue(item["executable"])
            self.assertEqual(item["operation"]["action"], "replace_range")
            self.assertEqual(item["operation"]["path"], "api.py")
            self.assertEqual(item["operation"]["source_sha256"], source_sha256)
            self.assertIn("_obsolete", item["operation"]["before"])

        equal_plan = self.generate(
            map_document, verification, ["SYMBOL-EQUAL"]
        )
        self.assertEqual(len(equal_plan["items"]), 1)
        suggestion = equal_plan["items"][0]
        self.assertEqual(len(suggestion["origins"]), 2)
        self.assertFalse(suggestion["executable"])
        self.assertEqual(
            suggestion["operation"]["action"], "rename_symbol"
        )
        self.assertEqual(suggestion["operation"]["symbol"], "_obsolete")
        self.assertEqual(suggestion["operation"]["new_name"], "missing")
        self.assertEqual(len(suggestion["operation"]["sites"]), 1)
        self.assertIn(
            "review it with --rename",
            suggestion["non_executable_reasons"][-1],
        )
        self.assertEqual(
            preview_plan(equal_plan, self.root, map_document)["status"],
            "blocked",
        )
        intent_laundering = json.loads(json.dumps(equal_plan))
        intent_laundering["items"][0]["executable"] = True
        intent_laundering["items"][0]["non_executable_reasons"] = []
        blocked = preview_plan(intent_laundering, self.root, map_document)
        self.assertEqual(blocked["status"], "blocked")
        self.assertIn(
            "requires asserted rename intent",
            blocked["diagnostics"][0]["message"],
        )

        incomplete = json.loads(json.dumps(verification))
        subset = next(
            row
            for row in incomplete["constraints"]
            if row["id"] == "SYMBOL-SUBSET"
        )
        actual_name = subset["operands"]["actual"]
        actual = next(
            row for row in incomplete["operands"] if row["name"] == actual_name
        )
        actual["completeness"]["classification"] = "partial"
        actual["completeness"]["exhaustive"] = False
        del incomplete["verification_result_sha256"]
        incomplete["verification_result_sha256"] = _digest(incomplete)
        guarded = self.generate(
            map_document, incomplete, ["SYMBOL-SUBSET"]
        )["items"][0]
        self.assertFalse(guarded["executable"])
        self.assertEqual(guarded["operation"]["action"], "manual")
        self.assertIn(
            "not current, complete",
            guarded["non_executable_reasons"][0],
        )

    def test_ambiguous_forbidden_symbol_is_manual_and_explicitly_unknown(
        self,
    ) -> None:
        files = []
        evidence = []
        for path in ("a.py", "b.py"):
            source = b"def obsolete():\n    return 1\n"
            sha256 = _write(self.root, path, source)
            files.append(
                _file(
                    path,
                    sha256,
                    symbols=[
                        {
                            "kind": "function",
                            "line": 1,
                            "name": "obsolete",
                            "scope": "function",
                            "signature": "def obsolete()",
                            "extent": {"start": 0, "end": len(source) - 1},
                        }
                    ],
                )
            )
            evidence.extend(_finding("obsolete", path=path, sha256=sha256)["evidence"])
        finding = _finding("obsolete")
        finding["evidence"] = evidence
        map_document = _map(files)
        check = _check(
            "NO-OBSOLETE",
            assertion="disjoint",
            actual="p.symbols",
            expected="l.symbols",
            findings=[finding],
        )
        verification = _verification(
            map_document,
            [check],
            {
                "p.symbols": {"select": "symbols", "names": ["obsolete"]},
                "l.symbols": {"kind": "literal_set", "values": ["obsolete"]},
            },
            [
                _symbol_operand(
                    "p.symbols", ["obsolete"], {"obsolete": evidence}
                ),
                _symbol_operand("l.symbols", ["obsolete"]),
            ],
        )

        plan = self.generate(map_document, verification)
        item = plan["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertEqual(item["operation"]["candidate_paths"], ["a.py", "b.py"])
        self.assertIn("found 2", item["non_executable_reasons"][0])
        self.assertEqual(item["unknowns"], [plan["unknowns"][0]["id"]])

    def test_required_path_never_invents_empty_content(self) -> None:
        map_document = _map([])
        check = _check(
            "REQUIRE-API",
            assertion="required_subset",
            actual="p.paths",
            expected="l.paths",
            findings=[
                _finding(
                    "generated/api.py",
                    comparison="missing",
                )
            ],
        )
        verification = _verification(
            map_document,
            [check],
            {
                "p.paths": {"select": "mapped_paths"},
                "l.paths": {
                    "kind": "literal_set",
                    "values": ["generated/api.py"],
                },
            },
        )

        plan = self.generate(map_document, verification)
        item = plan["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertEqual(
            item["operation"]["candidate_paths"], ["generated/api.py"]
        )
        self.assertNotIn("content", item["operation"])
        self.assertIn("does not specify file content", plan["unknowns"][0]["statement"])

    def test_incomplete_and_unsupported_constraints_remain_manual(self) -> None:
        source_sha256 = _write(self.root, "test_api.py", b"def test_api(): pass\n")
        map_document = _map([_file("test_api.py", source_sha256)])
        incomplete = _check(
            "MEMBERSHIP",
            assertion="numeric_bounds",
            actual="p.membership",
            status="unknown",
        )
        unsupported = _check(
            "TEST-ROUTE",
            assertion="min_test_routes",
            actual="p.routes",
            findings=[
                _finding(
                    "api:add", path="test_api.py", sha256=source_sha256
                )
            ],
        )
        verification = _verification(
            map_document,
            [incomplete, unsupported],
            {
                "p.membership": {
                    "select": "component_membership",
                    "include": ["src/**"],
                },
                "p.routes": {
                    "select": "test_routes",
                    "target_paths": ["api.py"],
                },
            },
        )

        plan = self.generate(map_document, verification)
        self.assertEqual(len(plan["items"]), 2)
        self.assertTrue(all(not item["executable"] for item in plan["items"]))
        self.assertEqual(
            {item["operation"]["action"] for item in plan["items"]},
            {"manual"},
        )
        self.assertEqual(len(plan["unknowns"]), 2)
        membership = plan["items"][0]
        self.assertEqual(
            membership["origins"][0]["issue_fingerprint"],
            membership["origins"][0]["constraint_result_sha256"],
        )

    def test_stale_source_lock_prevents_symbol_edit(self) -> None:
        mapped_source = b"def obsolete():\n    return 1\n"
        mapped_sha256 = hashlib.sha256(mapped_source).hexdigest()
        _write(self.root, "api.py", b"def obsolete():\n    return 2\n")
        map_document = _map(
            [
                _file(
                    "api.py",
                    mapped_sha256,
                    symbols=[
                        {
                            "kind": "function",
                            "line": 1,
                            "name": "obsolete",
                            "scope": "function",
                            "signature": "def obsolete()",
                            "extent": {
                                "start": 0,
                                "end": len(mapped_source) - 1,
                            },
                        }
                    ],
                )
            ]
        )
        check = _check(
            "NO-OBSOLETE",
            assertion="disjoint",
            actual="p.symbols",
            expected="l.symbols",
            findings=[
                _finding("obsolete", path="api.py", sha256=mapped_sha256)
            ],
        )
        verification = _verification(
            map_document,
            [check],
            {
                "p.symbols": {"select": "symbols", "names": ["obsolete"]},
                "l.symbols": {"kind": "literal_set", "values": ["obsolete"]},
            },
            [
                _symbol_operand(
                    "p.symbols",
                    ["obsolete"],
                    {
                        "obsolete": _finding(
                            "obsolete",
                            path="api.py",
                            sha256=mapped_sha256,
                        )["evidence"]
                    },
                ),
                _symbol_operand("l.symbols", ["obsolete"]),
            ],
        )

        item = self.generate(map_document, verification)["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertTrue(
            any(
                "no longer matches" in reason
                for reason in item["non_executable_reasons"]
            )
        )

    def test_symlink_path_is_never_marked_executable(self) -> None:
        source_sha256 = _write(self.root, "target.txt", b"target\n")
        (self.root / "legacy.txt").symlink_to("target.txt")
        map_document = _map([_file("legacy.txt", source_sha256)])
        operand, finding = _forbidden_path_operand(
            "p.paths", ["legacy.txt"]
        )
        check = _check(
            "NO-LEGACY",
            assertion="cardinality",
            actual="p.paths",
            exact=0,
            findings=[finding],
        )
        verification = _verification(
            map_document,
            [check],
            {"p.paths": {"select": "inventory_paths", "include": ["legacy.txt"]}},
            [operand],
        )

        item = self.generate(map_document, verification)["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "delete_file")
        self.assertTrue(
            any(
                "symbolic link" in reason
                for reason in item["non_executable_reasons"]
            )
        )

    def test_symlink_project_root_is_rejected(self) -> None:
        source_sha256 = _write(self.root, "source.py", b"VALUE = 1\n")
        map_document = _map([_file("source.py", source_sha256)])
        passing = _check(
            "KEEP-ARCHITECTURE",
            assertion="cardinality",
            actual="p.paths",
            status="pass",
            exact=0,
        )
        verification = _verification(
            map_document,
            [passing],
            {"p.paths": {"select": "inventory_paths", "include": ["legacy/**"]}},
        )
        linked_root = self.root.parent / f"{self.root.name}-link"
        linked_root.symlink_to(self.root, target_is_directory=True)
        self.addCleanup(linked_root.unlink)

        with self.assertRaisesRegex(ValueError, "symbolic link"):
            generate_plan(map_document, verification, None, linked_root)

    def test_tampered_or_cross_map_verification_is_rejected(self) -> None:
        map_document = _map([])
        check = _check(
            "UNKNOWN",
            assertion="cardinality",
            actual="p.unknown",
            status="unknown",
        )
        verification = _verification(
            map_document,
            [check],
            {"p.unknown": {"select": "constant_values"}},
        )

        tampered = json.loads(json.dumps(verification))
        tampered["constraints"][0]["rationale"] = "tampered"
        with self.assertRaisesRegex(ValueError, "digest"):
            generate_plan(map_document, tampered, None, self.root)

        cross_map = json.loads(json.dumps(verification))
        cross_map["evaluations"][0]["map_input_sha256"] = hashlib.sha256(
            b"other"
        ).hexdigest()
        cross_map["verification_result_sha256"] = _digest(
            {
                key: value
                for key, value in cross_map.items()
                if key != "verification_result_sha256"
            }
        )
        with self.assertRaisesRegex(ValueError, "map_input_sha256"):
            generate_plan(map_document, cross_map, None, self.root)

    def test_one_issue_fingerprint_produces_one_plan_item(self) -> None:
        make_sha256 = _write(
            self.root, "Makefile", b"WASM_EXPORTS = _core_add\n"
        )
        header_sha256 = _write(
            self.root,
            "src/core.h",
            b"int core_sum(int left, int right);\n",
        )
        map_document = _map(
            [
                _file("Makefile", make_sha256),
                _file("src/core.h", header_sha256),
            ]
        )
        unavailable = _finding(
            "core_sum",
            path="Makefile",
            sha256=make_sha256,
            comparison="different",
            evidence_state="unknown",
        )
        current = _finding(
            "core_sum",
            path="src/core.h",
            sha256=header_sha256,
            comparison="different",
        )
        current["fingerprint"] = unavailable["fingerprint"]
        check = _check(
            "PROVIDER-PARITY",
            assertion="mapped_values_equal",
            actual="p.surface",
            findings=[unavailable, current],
        )
        verification = _verification(
            map_document,
            [check],
            {
                "p.surface": {
                    "name": "ffi",
                    "select": "provider_surface",
                }
            },
        )

        plan = self.generate(map_document, verification)
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertEqual(
            item["operation"]["candidate_paths"],
            ["Makefile", "src/core.h"],
        )
        self.assertNotIn(
            "Finding evidence is waived, stale, inapplicable, or otherwise "
            "not current executable evidence.",
            item["non_executable_reasons"],
        )
        self.assertEqual(
            [row["path"] for row in item["evidence"]],
            ["Makefile", "src/core.h"],
        )
        self.assertEqual(len(plan["unknowns"]), 1)
        self.assertEqual(preview_plan(plan, self.root)["status"], "blocked")

        inconsistent = json.loads(json.dumps(verification))
        inconsistent["constraints"][0]["findings"][1]["key"] = "other"
        inconsistent["verification_result_sha256"] = _digest(
            {
                key: value
                for key, value in inconsistent.items()
                if key != "verification_result_sha256"
            }
        )
        with self.assertRaisesRegex(
            ValueError, "one issue fingerprint across distinct"
        ):
            generate_plan(map_document, inconsistent, None, self.root)

    def test_passing_verification_generates_valid_no_op_plan(self) -> None:
        unicode_sha256 = _write(
            self.root, "src/caf\u00e9.py", "VALUE = '\u2615'\n".encode()
        )
        map_document = _map([_file("src/caf\u00e9.py", unicode_sha256)])
        passing = _check(
            "KEEP-ARCHITECTURE",
            assertion="cardinality",
            actual="p.paths",
            status="pass",
            exact=0,
        )
        verification = _verification(
            map_document,
            [passing],
            {"p.paths": {"select": "inventory_paths", "include": ["legacy/**"]}},
        )

        plan = self.generate(map_document, verification)
        self.assertEqual(plan["items"], [])
        self.assertEqual(plan["unknowns"], [])
        self.assertEqual(
            plan["preserved_constraints"],
            ["KEEP-ARCHITECTURE"],
        )
        self.assertEqual(plan["source"]["map"]["sha256"], _digest(map_document))


if __name__ == "__main__":
    unittest.main()
