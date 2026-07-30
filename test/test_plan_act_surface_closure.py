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
from archbird.native import Project
from archbird.planning import generate_plan


BUILD = ROOT / "build"
FIXTURE = ROOT / "test/fixtures/plan_act/surface_closure"


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
    rows = verification["constraints"]
    assert isinstance(rows, list)
    by_id = {
        row["id"]: row
        for row in rows
        if isinstance(row, dict) and isinstance(row.get("id"), str)
    }
    expected = []
    for item in plan["items"]:
        assert isinstance(item, dict)
        acceptance = item["acceptance"]
        assert isinstance(acceptance, dict)
        for identifier in acceptance["constraints"]:
            if identifier not in expected:
                expected.append(identifier)
    for identifier in plan["preserved_constraints"]:
        if identifier not in expected:
            expected.append(identifier)
    if set(expected) != set(by_id):
        raise AssertionError("test Plan does not cover the complete policy")
    constraints = [
        {"id": identifier, "status": by_id[identifier]["status"]}
        for identifier in expected
    ]
    statuses = {row["status"] for row in constraints}
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
        "constraints": constraints,
    }


def _replacement_plan(
    plan: dict[str, object],
    source: bytes,
    replacement: str,
) -> dict[str, object]:
    result = copy.deepcopy(plan)
    start = source.index(b"core_add")
    end = start + len(b"core_add")
    item = result["items"][0]
    assert isinstance(item, dict)
    item["statement"] = "Update the stale Wasm export registration."
    item["provenance"] = "asserted"
    item["operation"] = {
        "action": "replace_range",
        "path": "Makefile",
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "start_byte": start,
        "end_byte": end,
        "before": "core_add",
        "replacement": replacement,
    }
    item["executable"] = True
    item["non_executable_reasons"] = []
    item["unknowns"] = []
    result["unknowns"] = []
    return result


class PlanActSurfaceClosureTest(unittest.TestCase):
    def setUp(self) -> None:
        BUILD.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(
            prefix="plan-act-product-", dir=BUILD
        )
        self.root = Path(self.temporary.name) / "repository"
        shutil.copytree(FIXTURE, self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_behavioral_gates(self) -> None:
        node = shutil.which("node")
        compiler = shutil.which("cc")
        if node is None or compiler is None:
            self.skipTest("Gate A requires node and a C compiler")
        completed = subprocess.run(
            [
                "make",
                "check",
                f"CC={compiler}",
                f"PYTHON={sys.executable}",
                f"NODE={node}",
            ],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)

    def test_architecture_acceptance_closes_a_gap_tests_do_not_cover(self) -> None:
        self.run_behavioral_gates()
        before_project = _project(self.root)
        before_map = before_project.map_json()
        before_verification = json.loads(before_project.verify_json())
        constraint = before_verification["constraints"][0]
        self.assertEqual(constraint["id"], "FFI-SURFACE")
        self.assertEqual(constraint["status"], "fail")
        self.assertEqual(len(constraint["findings"]), 1)
        finding = constraint["findings"][0]
        self.assertEqual(finding["comparison"], "unresolved")
        self.assertEqual(finding["key"], "core_add")
        self.assertEqual(
            [row["path"] for row in finding["evidence"]],
            ["Makefile"],
        )

        plan = generate_plan(
            json.loads(before_map),
            before_verification,
            None,
            self.root,
        )
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(item["operation"]["action"], "manual")
        self.assertEqual(item["operation"]["candidate_paths"], ["Makefile"])
        self.assertEqual(preview_plan(plan, self.root)["status"], "blocked")

        makefile = (self.root / "Makefile").read_bytes()
        incomplete = _replacement_plan(
            plan, makefile, "core_add _core_sum"
        )
        rejected = apply_plan(
            incomplete,
            self.root,
            _verification_acceptance,
            before_map,
        )
        self.assertEqual(rejected["status"], "rejected")
        self.assertEqual(rejected["acceptance"]["status"], "not_satisfied")
        self.assertEqual((self.root / "Makefile").read_bytes(), makefile)

        complete = _replacement_plan(plan, makefile, "core_sum")
        preview = preview_plan(complete, self.root, before_map)
        self.assertEqual(preview["status"], "preview")
        self.assertEqual(len(preview["changes"]), 1)
        self.assertEqual(preview["changes"][0]["path"], "Makefile")
        diff = preview["changes"][0]["unified_diff"]
        self.assertIn("-WASM_EXPORTS = _core_add", diff)
        self.assertIn("+WASM_EXPORTS = _core_sum", diff)
        self.assertEqual((self.root / "Makefile").read_bytes(), makefile)

        applied = apply_plan(
            complete,
            self.root,
            _verification_acceptance,
            before_map,
        )
        self.assertEqual(applied["status"], "applied")
        self.assertEqual(applied["acceptance"]["status"], "satisfied")
        after_verification = json.loads(_project(self.root).verify_json())
        self.assertEqual(after_verification["constraints"][0]["status"], "pass")
        self.run_behavioral_gates()


if __name__ == "__main__":
    unittest.main()
