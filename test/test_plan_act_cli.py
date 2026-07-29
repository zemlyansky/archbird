#!/usr/bin/env python3
"""Exercise the complete Verify -> Plan -> Act CLI lifecycle."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARCHBIRD = ROOT / "archbird"


def run(*arguments: str, expected: int = 0) -> subprocess.CompletedProcess[bytes]:
    completed = subprocess.run(
        [str(ARCHBIRD), *arguments],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"{' '.join(arguments)} exited {completed.returncode}, "
            f"expected {expected}\nstdout:\n"
            f"{completed.stdout.decode(errors='replace')}\nstderr:\n"
            f"{completed.stderr.decode(errors='replace')}"
        )
    return completed


class PlanActCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.root = Path(self.temporary.name)
        self.artifacts = self.root.parent / f"{self.root.name}-artifacts"
        self.artifacts.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()
        for path in sorted(self.artifacts.rglob("*"), reverse=True):
            if path.is_file() or path.is_symlink():
                path.unlink()
            elif path.is_dir():
                path.rmdir()
        self.artifacts.rmdir()

    def configure(self, constraints: dict[str, object]) -> None:
        (self.root / "archbird.json").write_text(
            json.dumps(
                {
                    "project": "plan-act-fixture",
                    "constraints": constraints,
                },
                sort_keys=True,
            )
        )

    def plan_path(self, name: str = "plan.json") -> Path:
        return self.artifacts / name

    def test_failing_constraint_generates_previews_and_applies_delete(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "Obsolete implementation stays absent.",
                }
            }
        )
        legacy = self.root / "legacy.py"
        legacy.write_text("def obsolete():\n    return 1\n")
        plan_path = self.plan_path()
        run(
            "plan",
            "NO-LEGACY",
            "--root",
            str(self.root),
            "--pretty",
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["items"][0]["operation"]["action"], "delete_file")
        self.assertTrue(plan["items"][0]["executable"])

        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn("--- a/legacy.py", preview)
        self.assertIn("+++ /dev/null", preview)
        self.assertTrue(legacy.exists(), "preview mutated the repository")

        result_path = self.plan_path("result.json")
        run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--apply",
            "--format",
            "json",
            "--pretty",
            "--output",
            str(result_path),
        )
        result = json.loads(result_path.read_bytes())
        self.assertEqual(result["status"], "applied")
        self.assertEqual(result["acceptance"]["status"], "satisfied")
        self.assertEqual(
            result["acceptance"]["constraints"],
            [{"id": "NO-LEGACY", "status": "pass"}],
        )
        self.assertFalse(legacy.exists())

    def test_plan_saved_in_repository_does_not_invalidate_itself(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "Obsolete implementation stays absent.",
                }
            }
        )
        legacy = self.root / "legacy.py"
        legacy.write_text("obsolete = True\n")
        plan_path = self.root / "plan.json"
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        first = json.loads(plan_path.read_bytes())
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        second = json.loads(plan_path.read_bytes())
        self.assertEqual(second["source"], first["source"])

        result = json.loads(
            run(
                "act",
                str(plan_path),
                "--root",
                str(self.root),
                "--apply",
                "--format",
                "json",
            ).stdout
        )
        self.assertEqual(result["status"], "applied")
        self.assertEqual(result["acceptance"]["status"], "satisfied")
        self.assertFalse(legacy.exists())

    def test_unrelated_source_drift_blocks_before_any_write(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "Obsolete implementation stays absent.",
                }
            }
        )
        legacy = self.root / "legacy.py"
        legacy.write_text("old = True\n")
        other = self.root / "other.py"
        other.write_text("value = 1\n")
        plan_path = self.plan_path()
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        other.write_text("value = 2\n")

        completed = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--apply",
            "--format",
            "json",
            expected=2,
        )
        result = json.loads(completed.stdout)
        self.assertEqual(result["status"], "blocked")
        self.assertIn("source context is stale", result["diagnostics"][0]["message"])
        self.assertTrue(legacy.exists())

    def test_missing_required_code_is_an_honest_non_executable_plan(self) -> None:
        self.configure(
            {
                "REQUIRED-API": {
                    "kind": "required_symbols",
                    "symbols": ["required_api"],
                    "owner": "architecture",
                    "rationale": "The public API remains available.",
                }
            }
        )
        (self.root / "module.py").write_text("def current_api():\n    return 1\n")
        plan_path = self.plan_path()
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["items"][0]["operation"]["action"], "manual")
        self.assertFalse(plan["items"][0]["executable"])
        before = (self.root / "module.py").read_bytes()

        result = json.loads(
            run(
                "act",
                str(plan_path),
                "--root",
                str(self.root),
                "--format",
                "json",
                expected=2,
            ).stdout
        )
        self.assertEqual(result["status"], "blocked")
        self.assertEqual((self.root / "module.py").read_bytes(), before)

    def test_passing_policy_produces_and_applies_no_op(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "Obsolete implementation stays absent.",
                }
            }
        )
        (self.root / "current.py").write_text("value = 1\n")
        plan_path = self.plan_path()
        run("plan", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["items"], [])

        result = json.loads(
            run(
                "act",
                str(plan_path),
                "--root",
                str(self.root),
                "--apply",
                "--format",
                "json",
            ).stdout
        )
        self.assertEqual(result["changes"], [])
        self.assertEqual(result["acceptance"]["status"], "satisfied")

    def test_selected_fix_that_breaks_passing_policy_is_rolled_back(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "Obsolete implementation stays absent.",
                },
                "KEEP-API": {
                    "kind": "required_symbols",
                    "symbols": ["required_api"],
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "The existing API remains available.",
                },
            }
        )
        legacy = self.root / "legacy.py"
        original = "def required_api():\n    return 1\n"
        legacy.write_text(original)
        plan_path = self.plan_path()
        run(
            "plan",
            "NO-LEGACY",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["preserved_constraints"], ["KEEP-API"])
        self.assertTrue(plan["items"][0]["executable"])

        completed = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--apply",
            "--format",
            "json",
            expected=1,
        )
        result = json.loads(completed.stdout)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["acceptance"]["status"], "not_satisfied")
        self.assertEqual(legacy.read_text(), original)

    def test_act_rejects_a_symlink_plan_locator(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["legacy.py"],
                    "owner": "architecture",
                    "rationale": "Legacy files stay absent.",
                }
            }
        )
        plan_path = self.plan_path()
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        link = self.plan_path("linked-plan.json")
        link.symlink_to(plan_path)
        completed = run(
            "act",
            str(link),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "must not be a symbolic link",
            completed.stderr.decode(),
        )


if __name__ == "__main__":
    unittest.main()
