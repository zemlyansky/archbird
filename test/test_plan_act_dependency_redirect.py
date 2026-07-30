from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from archbird.acting import apply_plan, preview_plan
from archbird.native import Project, evaluate_projection_json
from archbird.planning import generate_plan


BUILD = ROOT / "build"
FIXTURE = ROOT / "test/fixtures/plan_act/dependency_redirect"


def _project(root: Path) -> Project:
    return Project.from_repository(
        root,
        config=(root / "archbird.json").read_bytes(),
        jobs=1,
        cache_dir=None,
        map_cache=False,
    )


def _verification_acceptance(
    plan: dict[str, object], repository: Path
) -> dict[str, object]:
    verification = json.loads(_project(repository).verify_json())
    constraints = {
        row["id"]: row
        for row in verification["constraints"]
        if isinstance(row, dict) and isinstance(row.get("id"), str)
    }
    expected: list[str] = []
    for item in plan["items"]:
        for identifier in item["acceptance"]["constraints"]:
            if identifier not in expected:
                expected.append(identifier)
    for identifier in plan["preserved_constraints"]:
        if identifier not in expected:
            expected.append(identifier)
    if set(expected) != set(constraints):
        raise AssertionError("test Plan does not cover the complete policy")
    rows = [
        {"id": identifier, "status": constraints[identifier]["status"]}
        for identifier in expected
    ]
    statuses = {row["status"] for row in rows}
    status = (
        "not_satisfied"
        if "fail" in statuses
        else "unknown"
        if "unknown" in statuses
        else "satisfied"
    )
    return {
        "status": status,
        "verification_sha256": verification["verification_result_sha256"],
        "constraints": rows,
    }


def _asserted_redirect_plan(
    generated: dict[str, object],
    source: bytes,
    *,
    include: bool,
) -> dict[str, object]:
    result = copy.deepcopy(generated)
    template = result["items"][0]
    source_sha256 = hashlib.sha256(source).hexdigest()

    call_start = source.index(b"raw_value")
    call_item = copy.deepcopy(template)
    call_item.update(
        {
            "id": "redirect-ui-call",
            "statement": "Route the UI call through the service API.",
            "provenance": "asserted",
            "depends_on": ["redirect-ui-import"] if include else [],
            "executable": True,
            "non_executable_reasons": [],
            "operation": {
                "action": "replace_range",
                "path": "src/ui/view.c",
                "source_sha256": source_sha256,
                "start_byte": call_start,
                "end_byte": call_start + len(b"raw_value"),
                "before": "raw_value",
                "replacement": "service_value",
            },
            "unknowns": [],
        }
    )
    items = [call_item]
    if include:
        include_start = source.index(b"storage/raw.h")
        include_item = copy.deepcopy(template)
        include_item.update(
            {
                "id": "redirect-ui-import",
                "statement": "Replace the UI storage include with the service API.",
                "provenance": "asserted",
                "depends_on": [],
                "executable": True,
                "non_executable_reasons": [],
                "operation": {
                    "action": "replace_range",
                    "path": "src/ui/view.c",
                    "source_sha256": source_sha256,
                    "start_byte": include_start,
                    "end_byte": include_start + len(b"storage/raw.h"),
                    "before": "storage/raw.h",
                    "replacement": "service/api.h",
                },
                "unknowns": [],
            }
        )
        items.insert(0, include_item)
    result["provenance"] = "asserted"
    result["objective"] = "Route UI storage access through the service boundary."
    result["items"] = items
    result["unknowns"] = []
    return result


class PlanActDependencyRedirectTest(unittest.TestCase):
    def setUp(self) -> None:
        BUILD.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="plan-act-redirect-", dir=BUILD
        )
        self.root = Path(self.temporary.name) / "repository"
        shutil.copytree(FIXTURE, self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_behavioral_gate(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("dependency redirect test requires a C compiler")
        completed = subprocess.run(
            ["make", "check", f"CC={compiler}"],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)

    def test_exact_sites_bound_and_verify_a_dependency_redirect(self) -> None:
        self.run_behavioral_gate()
        before_project = _project(self.root)
        before_map_text = before_project.map_json()
        before_map = json.loads(before_map_text)
        self.assertGreaterEqual(before_map["schema_version"], 11)
        offending = [
            edge
            for edge in before_map["edges"]
            if edge["source"] == "src/ui/view.c"
            and edge["target"] == "src/storage/raw.h"
            and edge["kind"] == "import"
        ]
        self.assertEqual(len(offending), 1)
        self.assertEqual(
            offending[0]["sites"],
            [
                {
                    "fact_id": offending[0]["sites"][0]["fact_id"],
                    "line": 1,
                    "name": "storage/raw.h",
                    "path": "src/ui/view.c",
                    "span": {"end": 23, "start": 10},
                }
            ],
        )
        malformed = copy.deepcopy(before_map)
        malformed_edge = next(
            edge
            for edge in malformed["edges"]
            if edge["source"] == "src/ui/view.c"
            and edge["target"] == "src/storage/raw.h"
            and edge["kind"] == "import"
        )
        malformed_edge["sites"][0]["path"] = "src/storage/raw.h"
        with self.assertRaises(RuntimeError):
            evaluate_projection_json(
                json.dumps(
                    malformed, sort_keys=True, separators=(",", ":")
                ).encode(),
                {"id": "malformed-edge-site", "select": "file_edges"},
            )

        malformed = copy.deepcopy(before_map)
        malformed_edge = next(
            edge
            for edge in malformed["edges"]
            if edge["source"] == "src/ui/view.c"
            and edge["target"] == "src/storage/raw.h"
            and edge["kind"] == "import"
        )
        malformed_edge["sites"][0]["span"]["end"] = (
            next(
                row["bytes"]
                for row in malformed["files"]
                if row["path"] == "src/ui/view.c"
            )
            + 1
        )
        with self.assertRaises(RuntimeError):
            evaluate_projection_json(
                json.dumps(
                    malformed, sort_keys=True, separators=(",", ":")
                ).encode(),
                {"id": "out-of-range-edge-site", "select": "file_edges"},
            )

        verification = json.loads(before_project.verify_json())
        constraint = verification["constraints"][0]
        self.assertEqual(constraint["id"], "UI-STORAGE-BOUNDARY")
        self.assertEqual(constraint["status"], "fail")
        finding = constraint["findings"][0]
        self.assertEqual(finding["key"], "ui -[import]-> storage")
        derived = [
            row for row in finding["evidence"] if row["provenance"] == "derived"
        ]
        self.assertEqual(
            [(row["path"], row["line"]) for row in derived],
            [("src/ui/view.c", 1)],
        )

        plan = generate_plan(before_map, verification, None, self.root)
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertEqual(item["operation"]["candidate_paths"], ["src/ui/view.c"])
        self.assertEqual(
            item["operation"]["candidate_sites"],
            [
                {
                    "fact_id": offending[0]["sites"][0]["fact_id"],
                    "path": "src/ui/view.c",
                    "line": 1,
                    "source_sha256": hashlib.sha256(
                        (self.root / "src/ui/view.c").read_bytes()
                    ).hexdigest(),
                    "start_byte": 10,
                    "end_byte": 23,
                    "before": "storage/raw.h",
                    "name": "storage/raw.h",
                }
            ],
        )
        self.assertEqual(preview_plan(plan, self.root)["status"], "blocked")

        source_path = self.root / "src/ui/view.c"
        source = source_path.read_bytes()
        incomplete = _asserted_redirect_plan(plan, source, include=False)
        rejected = apply_plan(
            incomplete,
            self.root,
            _verification_acceptance,
            before_map_text,
        )
        self.assertEqual(rejected["status"], "rejected")
        self.assertEqual(rejected["acceptance"]["status"], "not_satisfied")
        self.assertEqual(source_path.read_bytes(), source)

        complete = _asserted_redirect_plan(plan, source, include=True)
        preview = preview_plan(complete, self.root, before_map_text)
        self.assertEqual(preview["status"], "preview")
        self.assertEqual(len(preview["changes"]), 1)
        diff = preview["changes"][0]["unified_diff"]
        self.assertIn('-#include "storage/raw.h"', diff)
        self.assertIn('+#include "service/api.h"', diff)
        self.assertIn("-int render_value(void) { return raw_value(); }", diff)
        self.assertIn("+int render_value(void) { return service_value(); }", diff)
        self.assertEqual(source_path.read_bytes(), source)

        applied = apply_plan(
            complete,
            self.root,
            _verification_acceptance,
            before_map_text,
        )
        self.assertEqual(applied["status"], "applied")
        self.assertEqual(applied["acceptance"]["status"], "satisfied")
        after_project = _project(self.root)
        after_verification = json.loads(after_project.verify_json())
        self.assertEqual(after_verification["constraints"][0]["status"], "pass")
        after_map = json.loads(after_project.map_json())
        self.assertNotIn(
            ("src/ui/view.c", "src/storage/raw.h"),
            {(edge["source"], edge["target"]) for edge in after_map["edges"]},
        )
        self.assertIn(
            ("src/ui/view.c", "src/service/api.h"),
            {(edge["source"], edge["target"]) for edge in after_map["edges"]},
        )
        self.run_behavioral_gate()

    def test_repeated_imports_remain_distinct_exact_sites(self) -> None:
        source_path = self.root / "src/ui/view.c"
        source_path.write_text(
            '#include "storage/raw.h"\n'
            '#include "storage/raw.h"\n\n'
            "int render_value(void) { return raw_value(); }\n",
            encoding="utf-8",
        )
        project = _project(self.root)
        map_document = json.loads(project.map_json())
        edge = next(
            row
            for row in map_document["edges"]
            if row["source"] == "src/ui/view.c"
            and row["target"] == "src/storage/raw.h"
        )
        self.assertEqual(
            [
                (site["line"], site["span"]["start"], site["span"]["end"])
                for site in edge["sites"]
            ],
            [(1, 10, 23), (2, 35, 48)],
        )
        duplicate = copy.deepcopy(map_document)
        duplicated_edge = next(
            row
            for row in duplicate["edges"]
            if row["source"] == "src/ui/view.c"
            and row["target"] == "src/storage/raw.h"
        )
        duplicated_edge["sites"].append(
            copy.deepcopy(duplicated_edge["sites"][0])
        )
        with self.assertRaises(RuntimeError):
            evaluate_projection_json(
                json.dumps(
                    duplicate, sort_keys=True, separators=(",", ":")
                ).encode(),
                {"id": "duplicate-edge-site", "select": "file_edges"},
            )
        verification = json.loads(project.verify_json())
        finding = verification["constraints"][0]["findings"][0]
        derived = [
            row for row in finding["evidence"] if row["provenance"] == "derived"
        ]
        self.assertEqual(
            [(row["path"], row["line"]) for row in derived],
            [("src/ui/view.c", 1), ("src/ui/view.c", 2)],
        )
        plan = generate_plan(
            map_document,
            verification,
            None,
            self.root,
        )
        self.assertEqual(
            [
                (
                    site["line"],
                    site["start_byte"],
                    site["end_byte"],
                    site["before"],
                )
                for site in plan["items"][0]["operation"]["candidate_sites"]
            ],
            [
                (1, 10, 23, "storage/raw.h"),
                (2, 35, 48, "storage/raw.h"),
            ],
        )

    def test_repeated_external_calls_remain_distinct_exact_sites(self) -> None:
        config_path = self.root / "archbird.json"
        config = json.loads(config_path.read_text(encoding="utf-8"))
        config["layers"][0]["external_call_namespaces"] = [
            {"prefix": "external_", "package": "external-runtime"}
        ]
        config_path.write_text(
            json.dumps(config, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        source_path = self.root / "src/ui/view.c"
        source = (
            b"int external_api(void);\n"
            b"int render_value(void) { return external_api() + external_api(); }\n"
        )
        source_path.write_bytes(source)

        map_document = json.loads(_project(self.root).map_json())
        edge = next(
            row
            for row in map_document["edges"]
            if row["source"] == "src/ui/view.c"
            and row["target"] == "package:external-runtime"
            and row["kind"] == "external-call"
        )
        first = source.index(b"external_api", source.index(b"return"))
        second = source.index(b"external_api", first + 1)
        self.assertEqual(
            [
                (site["line"], site["span"]["start"], site["span"]["end"])
                for site in edge["sites"]
            ],
            [
                (2, first, first + len(b"external_api")),
                (2, second, second + len(b"external_api")),
            ],
        )


if __name__ == "__main__":
    unittest.main()
