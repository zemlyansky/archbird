#!/usr/bin/env python3
"""Exercise the complete Verify -> Plan -> Act -> Apply lifecycle."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from archbird import act_transport

ROOT = Path(__file__).resolve().parents[1]
ARCHBIRD = ROOT / "archbird"
SURFACE_FIXTURE = ROOT / "test/fixtures/plan_act/surface_closure"
REGISTRATION_FIXTURE = ROOT / "test/fixtures/plan_act/surface_registration"
REDIRECT_FIXTURE = ROOT / "test/fixtures/plan_act/dependency_redirect"


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

    def accepted_act(
        self,
        plan_path: Path,
        name: str = "act.json",
        *,
        root: Path | None = None,
    ) -> tuple[Path, dict[str, object]]:
        repository = root or self.root
        act_path = self.plan_path(name)
        run(
            "act",
            str(plan_path),
            "--root",
            str(repository),
            "--format",
            "json",
            "--output",
            str(act_path),
        )
        act = json.loads(act_path.read_bytes())
        self.assertEqual(act["artifact"], "act")
        self.assertEqual(act["state"], "accepted")
        self.assertEqual(act["acceptance"]["status"], "satisfied")
        return act_path, act

    def run_surface_gates(self, root: Path | None = None) -> None:
        repository = root or self.root
        compiler = shutil.which("cc")
        node = shutil.which("node")
        make = shutil.which("make")
        if compiler is None or node is None or make is None:
            self.skipTest("surface closure requires make, cc, and node")
        completed = subprocess.run(
            [
                make,
                "check",
                f"CC={compiler}",
                f"PYTHON={sys.executable}",
                f"NODE={node}",
            ],
            cwd=repository,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout.decode())

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

        act_path, patch = self.accepted_act(plan_path)
        self.assertTrue(legacy.exists(), "Act mutated the repository")
        self.assertEqual(
            patch["acceptance"]["constraints"],
            [{"id": "NO-LEGACY", "status": "pass"}],
        )
        original = legacy.read_bytes()
        legacy.write_text("drifted = True\n")
        drift = run(
            "apply",
            str(act_path),
            "--root",
            str(self.root),
            expected=2,
        )
        self.assertIn("legacy.py", drift.stderr.decode())
        self.assertEqual(legacy.read_text(), "drifted = True\n")
        legacy.write_bytes(original)
        run("apply", str(act_path), "--root", str(self.root))
        self.assertFalse(legacy.exists())
        replay = run(
            "apply",
            str(act_path),
            "--root",
            str(self.root),
            expected=2,
        )
        self.assertIn("legacy.py", replay.stderr.decode())

    def test_forbidden_top_level_symbol_is_removed_exactly(self) -> None:
        self.configure(
            {
                "NO-OBSOLETE-SYMBOL": {
                    "kind": "forbidden_symbols",
                    "symbols": ["obsolete"],
                    "paths": ["module.py"],
                    "owner": "architecture",
                    "rationale": "The obsolete API stays absent.",
                }
            }
        )
        source = self.root / "module.py"
        source.write_text(
            "def obsolete():\n"
            "    return 1\n\n"
            "def retained():\n"
            "    return 2\n"
        )
        plan_path = self.plan_path()
        run(
            "plan",
            "NO-OBSOLETE-SYMBOL",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 1)
        operation = plan["items"][0]["operation"]
        self.assertEqual(operation["action"], "replace_range")
        self.assertEqual(operation["path"], "module.py")
        self.assertEqual(
            operation["before"],
            "def obsolete():\n    return 1",
        )
        self.assertEqual(operation["replacement"], "")

        act_path, _ = self.accepted_act(plan_path)
        self.assertIn("def obsolete", source.read_text())
        run("apply", str(act_path), "--root", str(self.root))
        self.assertNotIn("def obsolete", source.read_text())
        self.assertIn("def retained", source.read_text())

    def test_reviewed_provider_surface_rename_closes_exact_make_input(self) -> None:
        shutil.copytree(SURFACE_FIXTURE, self.root, dirs_exist_ok=True)
        self.run_surface_gates()
        failed = run(
            "verify",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        self.assertIn("FFI-SURFACE", failed.stdout.decode())

        manual_path = self.plan_path("manual-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(manual_path),
        )
        manual = json.loads(manual_path.read_bytes())
        self.assertFalse(manual["items"][0]["executable"])
        self.assertEqual(manual["items"][0]["operation"]["action"], "manual")

        makefile = self.root / "Makefile"
        original = makefile.read_bytes()
        makefile.write_bytes(
            original.replace(b"_core_add", b"_core_add _core_obsolete")
        )
        two_stale_path = self.plan_path("two-stale-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(two_stale_path),
        )
        two_stale = json.loads(two_stale_path.read_bytes())
        self.assertTrue(two_stale["items"])
        self.assertTrue(
            all(not item["executable"] for item in two_stale["items"]),
            "unresolved provider entries cannot prove each other safe to remove",
        )
        makefile.write_bytes(original)

        unknown_target = run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--rename",
            "core_add=core_missing",
            "--output",
            str(self.plan_path("unknown-target.json")),
            expected=2,
        )
        self.assertIn(
            "asserted rename does not match",
            unknown_target.stderr.decode(),
        )

        makefile.write_bytes(original.replace(b"_core_add", b"_core_add _core_add"))
        duplicate = run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--rename",
            "core_add=core_sum",
            "--output",
            str(self.plan_path("duplicate-token.json")),
            expected=2,
        )
        self.assertIn(
            "Make variable token edit expected one match but found 2",
            duplicate.stderr.decode(),
        )
        makefile.write_bytes(original)

        plan_path = self.plan_path("surface-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--rename",
            "core_add=core_sum",
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(item["provenance"], "asserted")
        self.assertEqual(
            item["operation"],
            {
                "action": "edit_make_variable_token",
                "expected_token": "_core_add",
                "path": "Makefile",
                "replacement_token": "_core_sum",
                "source_sha256": hashlib.sha256(original).hexdigest(),
                "variable": "WASM_EXPORTS",
            },
        )
        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn("-WASM_EXPORTS = _core_add", preview)
        self.assertIn("+WASM_EXPORTS = _core_sum", preview)
        self.assertEqual(makefile.read_bytes(), original)

        act_path, patch = self.accepted_act(
            plan_path, "surface-act.json"
        )
        self.assertEqual(
            patch["acceptance"]["constraints"],
            [{"id": "FFI-SURFACE", "status": "pass"}],
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn("WASM_EXPORTS = _core_sum", makefile.read_text())
        run("verify", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_before_map_derives_residual_provider_surface_rename(self) -> None:
        shutil.copytree(SURFACE_FIXTURE, self.root, dirs_exist_ok=True)
        before_root = self.artifacts / "before"
        shutil.copytree(SURFACE_FIXTURE, before_root)
        for relative in (
            "src/core.c",
            "src/core.h",
            "src/test_core.c",
            "py/api.py",
            "py/test_api.py",
            "js/runtime.js",
            "js/test_api.js",
        ):
            path = before_root / relative
            path.write_text(path.read_text().replace("core_sum", "core_add"))
        before_map_path = self.plan_path("observed-before-map.json")
        run(
            "map",
            "--root",
            str(before_root),
            "--format",
            "json",
            "--output",
            str(before_map_path),
            "--check",
        )
        before_map = before_map_path.read_bytes()

        incompatible = json.loads(before_map)
        incompatible["project"] = "different-project"
        incompatible_path = self.plan_path("incompatible-before-map.json")
        incompatible_path.write_text(
            json.dumps(incompatible, sort_keys=True, separators=(",", ":"))
        )
        rejected = run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--before-map",
            str(incompatible_path),
            "--output",
            str(self.plan_path("rejected-observed-plan.json")),
            expected=2,
        )
        self.assertIn(
            "before and current Maps have incompatible identities",
            rejected.stderr.decode(),
        )

        ambiguous_root = self.artifacts / "ambiguous"
        shutil.copytree(SURFACE_FIXTURE, ambiguous_root)
        core = ambiguous_root / "src/core.c"
        core.write_text(
            core.read_text()
            + "\nint core_total(int left, int right) { return left + right; }\n"
        )
        header = ambiguous_root / "src/core.h"
        header.write_text(
            header.read_text().replace(
                "\n#endif", "\nint core_total(int left, int right);\n\n#endif"
            )
        )
        runtime = ambiguous_root / "js/runtime.js"
        runtime.write_text(
            runtime.read_text()
            .replace(
                "module.exports = { add };",
                "function total(wasm, left, right) {\n"
                "  return wasm._core_total(left, right);\n"
                "}\n\nmodule.exports = { add, total };",
            )
        )
        js_test = ambiguous_root / "js/test_api.js"
        js_test.write_text(
            js_test.read_text()
            .replace(
                'const { add } = require("./runtime");',
                'const { add, total } = require("./runtime");',
            )
            .replace(
                "    _core_sum(left, right) {",
                "    _core_total(left, right) {\n"
                "      return left + right;\n"
                "    },\n"
                "    _core_sum(left, right) {",
            )
            .replace(
                "  assert.equal(add(wasm, 2, 3), 5);",
                "  assert.equal(add(wasm, 2, 3), 5);\n"
                "  assert.equal(total(wasm, 2, 3), 5);",
            )
        )
        python_api = ambiguous_root / "py/api.py"
        python_api.write_text(
            python_api.read_text()
            + "\n\ndef total(backend, left, right):\n"
            "    return backend.core_total(left, right)\n"
        )
        python_test = ambiguous_root / "py/test_api.py"
        python_test.write_text(
            python_test.read_text()
            .replace("from api import add", "from api import add, total")
            .replace(
                "    def core_sum(left, right):",
                "    def core_total(left, right):\n"
                "        return left + right\n\n"
                "    @staticmethod\n"
                "    def core_sum(left, right):",
            )
            .replace(
                "        self.assertEqual(add(Backend(), 2, 3), 5)",
                "        self.assertEqual(add(Backend(), 2, 3), 5)\n"
                "        self.assertEqual(total(Backend(), 2, 3), 5)",
            )
        )
        ambiguous_plan_path = self.plan_path("ambiguous-observed-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(ambiguous_root),
            "--before-map",
            str(before_map_path),
            "--output",
            str(ambiguous_plan_path),
        )
        ambiguous_plan = json.loads(ambiguous_plan_path.read_bytes())
        self.assertTrue(ambiguous_plan["items"])
        self.assertTrue(
            all(not item["executable"] for item in ambiguous_plan["items"]),
            "multiple observed rename targets must not authorize an edit",
        )

        plan_path = self.plan_path("observed-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--before-map",
            str(before_map_path),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["schema_version"], 2)
        canonical_before_map = json.dumps(
            json.loads(before_map),
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        self.assertEqual(
            plan["source"]["before_map"]["sha256"],
            hashlib.sha256(canonical_before_map).hexdigest(),
        )
        self.assertNotEqual(
            plan["source"]["before_map"]["input_sha256"],
            plan["source"]["map"]["input_sha256"],
        )
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(item["provenance"], "derived")
        self.assertEqual(
            item["statement"],
            "Replace stale provider registration core_add with core_sum in "
            "Makefile.",
        )
        self.assertEqual(
            item["operation"]["action"], "edit_make_variable_token"
        )
        self.assertEqual(item["operation"]["expected_token"], "_core_add")
        self.assertEqual(item["operation"]["replacement_token"], "_core_sum")

        act_path, patch = self.accepted_act(
            plan_path, "observed-act.json"
        )
        self.assertEqual(
            patch["acceptance"]["constraints"],
            [{"id": "FFI-SURFACE", "status": "pass"}],
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn(
            "WASM_EXPORTS = _core_sum",
            (self.root / "Makefile").read_text(),
        )
        run("verify", "FFI-SURFACE", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_git_diff_derives_residual_plan_without_mutating_worktree(self) -> None:
        project = self.root / "packages" / "surface"
        shutil.copytree(SURFACE_FIXTURE, project)
        renamed_sources = (
            "src/core.c",
            "src/core.h",
            "src/test_core.c",
            "py/api.py",
            "py/test_api.py",
            "js/runtime.js",
            "js/test_api.js",
        )
        for relative in renamed_sources:
            path = project / relative
            path.write_text(path.read_text().replace("core_sum", "core_add"))
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=Archbird Test",
                "-c",
                "user.email=archbird@example.invalid",
                "commit",
                "-qm",
                "before migration",
            ],
            cwd=self.root,
            check=True,
        )
        for relative in renamed_sources:
            path = project / relative
            path.write_text(path.read_text().replace("core_add", "core_sum"))
        before_status = subprocess.run(
            ["git", "status", "--porcelain=v1", "-z"],
            cwd=self.root,
            check=True,
            stdout=subprocess.PIPE,
        ).stdout

        plan_path = self.plan_path("git-residual-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(project),
            "--git-diff",
            "HEAD",
            "--output",
            str(plan_path),
        )
        self.assertEqual(
            subprocess.run(
                ["git", "status", "--porcelain=v1", "-z"],
                cwd=self.root,
                check=True,
                stdout=subprocess.PIPE,
            ).stdout,
            before_status,
            "Git-derived planning mutated the working tree",
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 1)
        self.assertEqual(plan["items"][0]["provenance"], "derived")
        self.assertEqual(
            plan["items"][0]["operation"]["expected_token"],
            "_core_add",
        )
        self.assertEqual(
            plan["items"][0]["operation"]["replacement_token"],
            "_core_sum",
        )

        act_path, _ = self.accepted_act(
            plan_path, "git-residual-act.json", root=project
        )
        run("apply", str(act_path), "--root", str(project))
        run("verify", "FFI-SURFACE", "--root", str(project), "--check")
        self.run_surface_gates(project)

    def test_required_provider_surface_derives_exact_make_insertion(self) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        self.run_surface_gates()
        failed = run(
            "verify",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        self.assertIn("provider does not declare core_sum", failed.stdout.decode())
        self.assertIn(
            "used provider capability is not declared: core_sum",
            failed.stdout.decode(),
        )

        makefile = self.root / "Makefile"
        original = makefile.read_bytes()
        plan_path = self.plan_path("registration-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(item["provenance"], "derived")
        self.assertEqual(len(item["origins"]), 2)
        self.assertEqual(
            item["operation"],
            {
                "action": "insert_make_variable_token",
                "anchor_token": "_core_peer",
                "path": "Makefile",
                "position": "after",
                "source_sha256": hashlib.sha256(original).hexdigest(),
                "token": "_core_sum",
                "variable": "WASM_EXPORTS",
            },
        )
        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn("-WASM_EXPORTS = _core_peer", preview)
        self.assertIn("+WASM_EXPORTS = _core_peer _core_sum", preview)
        self.assertEqual(makefile.read_bytes(), original)

        act_path, patch = self.accepted_act(
            plan_path, "registration-act.json"
        )
        self.assertEqual(
            patch["acceptance"]["constraints"],
            [
                {"id": "FFI-SURFACE", "status": "pass"},
                {"id": "REQUIRED-HEADER", "status": "pass"},
            ],
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn("WASM_EXPORTS = _core_peer _core_sum", makefile.read_text())
        run("verify", "FFI-SURFACE", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_required_c_declaration_and_registration_form_one_plan(self) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        implementation = self.root / "src/core.c"
        implementation.write_text(
            implementation.read_text().replace(
                "int core_sum(int left, int right)",
                "int core_sum(int left,int right)",
            )
        )
        (self.root / "js/decoy.js").write_text(
            "function core_sum(left, right) { return left + right; }\n"
        )
        header = self.root / "src/core.h"
        header.write_text(
            header.read_text().replace(
                "int core_sum(int left, int right);\n", ""
            )
        )
        original_header = header.read_bytes()
        original_makefile = (self.root / "Makefile").read_bytes()
        failed = run(
            "verify",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        report = failed.stdout.decode()
        self.assertIn("REQUIRED-HEADER", report)
        self.assertIn("FFI-SURFACE", report)

        plan_path = self.plan_path("coordinated-surface-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 2)
        operations = {
            item["operation"]["action"]: item["operation"]
            for item in plan["items"]
        }
        declaration = operations["insert_c_declaration"]
        self.assertEqual(declaration["path"], "src/core.h")
        self.assertEqual(declaration["symbol"], "core_sum")
        self.assertEqual(
            declaration["signature"], "int core_sum(int left,int right)"
        )
        self.assertTrue(declaration["anchor_fact_id"].startswith("f:"))
        self.assertEqual(
            declaration["source_sha256"],
            hashlib.sha256(original_header).hexdigest(),
        )
        registration = operations["insert_make_variable_token"]
        self.assertEqual(registration["token"], "_core_sum")
        self.assertEqual(
            registration["source_sha256"],
            hashlib.sha256(original_makefile).hexdigest(),
        )

        act_path, patch = self.accepted_act(
            plan_path, "coordinated-surface-act.json"
        )
        self.assertEqual(len(patch["transitions"]), 2)
        self.assertEqual(header.read_bytes(), original_header)
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn(
            "int core_sum(int left,int right);", header.read_text()
        )
        self.assertIn(
            "WASM_EXPORTS = _core_peer _core_sum",
            (self.root / "Makefile").read_text(),
        )
        run("verify", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_c_declaration_requires_proven_style_and_rechecks_signature(
        self,
    ) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        makefile = self.root / "Makefile"
        makefile.write_text(
            makefile.read_text().replace(
                "WASM_EXPORTS = _core_peer",
                "WASM_EXPORTS = _core_peer _core_sum",
            )
        )
        header = self.root / "src/core.h"
        header.write_text(
            header.read_text()
            .replace("int core_sum(int left, int right);\n", "")
            .replace(
                "int core_peer(int left, int right);",
                "extern int core_peer(int left, int right);",
            )
        )
        manual_path = self.plan_path("manual-header-plan.json")
        run(
            "plan",
            "REQUIRED-HEADER",
            "--root",
            str(self.root),
            "--output",
            str(manual_path),
        )
        manual = json.loads(manual_path.read_bytes())
        self.assertEqual(len(manual["items"]), 1)
        self.assertFalse(manual["items"][0]["executable"])
        self.assertEqual(manual["items"][0]["operation"]["action"], "manual")
        self.assertIn(
            "signature style",
            " ".join(manual["items"][0]["non_executable_reasons"]),
        )

        header.write_text(
            header.read_text().replace(
                "extern int core_peer", "int core_peer"
            )
        )
        plan_path = self.plan_path("tampered-header-plan.json")
        run(
            "plan",
            "REQUIRED-HEADER",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertTrue(plan["items"][0]["executable"])
        plan["items"][0]["operation"]["signature"] = "int core_sum(void)"
        plan_path.write_text(json.dumps(plan, sort_keys=True))
        rejected = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "insert_c_declaration differs from its Map proof",
            rejected.stderr.decode(),
        )

        implementation = self.root / "src/core.c"
        implementation.write_text(
            implementation.read_text().replace(
                "int core_sum(int left, int right)",
                "static int core_sum(int left, int right)",
            )
        )
        static_path = self.plan_path("static-header-plan.json")
        run(
            "plan",
            "REQUIRED-HEADER",
            "--root",
            str(self.root),
            "--output",
            str(static_path),
        )
        static_plan = json.loads(static_path.read_bytes())
        self.assertFalse(static_plan["items"][0]["executable"])
        self.assertIn(
            "safe external declaration",
            " ".join(static_plan["items"][0]["non_executable_reasons"]),
        )

    def test_multiple_required_registrations_compose_one_file_transition(
        self,
    ) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        core = self.root / "src/core.c"
        core.write_text(
            core.read_text().replace(
                "int core_peer",
                "int core_product(int left, int right) { return left * right; }\n\n"
                "int core_peer",
            )
        )
        header = self.root / "src/core.h"
        header.write_text(
            header.read_text().replace(
                "int core_peer",
                "int core_product(int left, int right);\nint core_peer",
            )
        )
        runtime = self.root / "js/runtime.js"
        runtime.write_text(
            runtime.read_text().replace(
                "module.exports = { add };",
                "function multiply(wasm, left, right) {\n"
                "  return wasm._core_product(left, right);\n"
                "}\n\n"
                "module.exports = { add, multiply };",
            )
        )
        config_path = self.root / "archbird.json"
        config = json.loads(config_path.read_bytes())
        config["constraints"]["FFI-SURFACE"]["declared"] = [
            "core_product",
            "core_sum",
        ]
        config_path.write_text(json.dumps(config, sort_keys=True))
        self.run_surface_gates()

        failed = run(
            "verify",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        output = failed.stdout.decode()
        self.assertIn("provider does not declare core_product", output)
        self.assertIn("provider does not declare core_sum", output)

        plan_path = self.plan_path("multiple-registration-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 2)
        self.assertTrue(all(item["executable"] for item in plan["items"]))
        self.assertEqual(
            {
                item["operation"]["token"]
                for item in plan["items"]
            },
            {"_core_product", "_core_sum"},
        )

        act_path, patch = self.accepted_act(
            plan_path, "multiple-registration-act.json"
        )
        self.assertEqual(len(patch["transitions"]), 1)
        self.assertEqual(
            set(patch["transitions"][0]["item_ids"]),
            {item["id"] for item in plan["items"]},
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn(
            "WASM_EXPORTS = _core_peer _core_product _core_sum",
            (self.root / "Makefile").read_text(),
        )
        run("verify", "FFI-SURFACE", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_unused_provider_registration_derives_exact_make_removal(self) -> None:
        shutil.copytree(SURFACE_FIXTURE, self.root, dirs_exist_ok=True)
        makefile = self.root / "Makefile"
        original = makefile.read_bytes().replace(
            b"WASM_EXPORTS = _core_add",
            b"WASM_EXPORTS = _core_sum _core_add",
        )
        makefile.write_bytes(original)
        self.run_surface_gates()
        failed = run(
            "verify",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        self.assertIn(
            "provider capability is unresolved: core_add",
            failed.stdout.decode(),
        )

        plan_path = self.plan_path("removal-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(item["provenance"], "derived")
        self.assertEqual(
            item["statement"],
            "Remove stale provider registration core_add from Makefile.",
        )
        self.assertEqual(
            item["operation"],
            {
                "action": "edit_make_variable_token",
                "expected_token": "_core_add",
                "path": "Makefile",
                "replacement_token": "",
                "source_sha256": hashlib.sha256(original).hexdigest(),
                "variable": "WASM_EXPORTS",
            },
        )
        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn("-WASM_EXPORTS = _core_sum _core_add", preview)
        self.assertIn("+WASM_EXPORTS = _core_sum", preview)
        self.assertEqual(makefile.read_bytes(), original)

        act_path, patch = self.accepted_act(plan_path, "removal-act.json")
        self.assertEqual(
            patch["acceptance"]["constraints"],
            [{"id": "FFI-SURFACE", "status": "pass"}],
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn("WASM_EXPORTS = _core_sum\n", makefile.read_text())
        run("verify", "FFI-SURFACE", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_map_query_verify_plan_act_closes_a_multifile_rename(self) -> None:
        (self.root / "api.py").write_text(
            "def old_api(value):\n    return value + 1\n"
        )
        (self.root / "consumer.py").write_text(
            "from api import old_api\n"
            "result = old_api(1)\n"
        )
        (self.root / "archbird.json").write_text(
            json.dumps(
                {
                    "project": "plan-act-rename",
                    "layers": [
                        {
                            "name": "python",
                            "language": "python",
                            "globs": ["*.py"],
                            "import_roots": ["."],
                        }
                    ],
                    "projections": {
                        "api-symbols": {
                            "select": "symbols",
                            "paths": ["api.py"],
                        }
                    },
                    "constraints": {
                        "API-SURFACE": {
                            "assert": "set_equal",
                            "actual": {"projection": "api-symbols"},
                            "expected": {"literal": ["new_api"]},
                            "owner": "architecture",
                            "rationale": "The reviewed API rename is complete.",
                        }
                    },
                },
                sort_keys=True,
            )
        )
        map_path = self.plan_path("before-map.json")
        run(
            "map",
            "--root",
            str(self.root),
            "--format",
            "json",
            "--output",
            str(map_path),
            "--check",
        )
        before_query = run(
            "query",
            "--root",
            str(self.root),
            "--symbol",
            "old_api",
            "--format",
            "json",
            "--check",
        )
        self.assertIn(b'"old_api"', before_query.stdout)
        run("verify", "--root", str(self.root), "--check", expected=1)

        plan_path = self.plan_path("rename-plan.json")
        run(
            "plan",
            "API-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        suggestion = json.loads(plan_path.read_bytes())["items"][0]
        self.assertEqual(suggestion["operation"]["action"], "rename_symbol")
        self.assertFalse(suggestion["executable"])
        run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        run(
            "plan",
            "API-SURFACE",
            "--root",
            str(self.root),
            "--rename",
            "old_api=new_api",
            "--output",
            str(plan_path),
        )
        plan_document = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan_document["items"]), 1)
        item = plan_document["items"][0]
        self.assertTrue(item["executable"])
        self.assertEqual(item["operation"]["action"], "rename_symbol")
        self.assertEqual(
            item["operation"]["projection"]["select"],
            "symbol_occurrences",
        )
        self.assertEqual(
            {row["path"] for row in item["operation"]["sites"]},
            {"api.py", "consumer.py"},
        )
        self.assertEqual(len(item["operation"]["sites"]), 3)

        patch = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn("--- a/api.py", patch)
        self.assertIn("--- a/consumer.py", patch)
        act_path, _patch = self.accepted_act(
            plan_path, "rename-act.json"
        )
        self.assertIn("old_api", (self.root / "api.py").read_text())
        run("apply", str(act_path), "--root", str(self.root))
        self.assertNotIn("old_api", (self.root / "api.py").read_text())
        self.assertNotIn("old_api", (self.root / "consumer.py").read_text())
        run("verify", "--root", str(self.root), "--check")
        after_query = run(
            "query",
            "--root",
            str(self.root),
            "--symbol",
            "new_api",
            "--format",
            "json",
            "--check",
        )
        self.assertIn(b'"new_api"', after_query.stdout)

    def test_neutral_plan_is_grounded_by_the_c_dependency_executor(self) -> None:
        shutil.copytree(REDIRECT_FIXTURE, self.root, dirs_exist_ok=True)
        plan_path = self.plan_path("dependency-redirect-plan.json")
        failed = run(
            "verify",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        self.assertIn("UI-STORAGE-BOUNDARY", failed.stdout.decode())
        run(
            "plan",
            "UI-STORAGE-BOUNDARY",
            "--root",
            str(self.root),
            "--redirect",
            "raw_value=service_value",
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        operation = plan["items"][0]["operation"]
        self.assertEqual(operation["action"], "redirect_dependency")
        self.assertEqual(operation["from_symbol"], "raw_value")
        self.assertEqual(operation["to_symbol"], "service_value")
        self.assertEqual(operation["relation"], "ui -[import]-> storage")
        self.assertEqual(operation["source_paths"], ["src/ui/view.c"])
        self.assertEqual(
            operation["projection"],
            {"kinds": ["import"], "select": "component_edges"},
        )
        for grounded in (
            "before",
            "end_byte",
            "replacement",
            "sites",
            "source_sha256",
            "start_byte",
        ):
            self.assertNotIn(grounded, operation)

        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn('-#include "storage/raw.h"', preview)
        self.assertIn('+#include "service/api.h"', preview)
        self.assertIn("-int render_value(void) { return raw_value(); }", preview)
        self.assertIn(
            "+int render_value(void) { return service_value(); }", preview
        )
        self.assertIn("raw_value", (self.root / "src/ui/view.c").read_text())

        act_path, act = self.accepted_act(
            plan_path, "dependency-redirect-act.json"
        )
        self.assertEqual(act["artifact"], "act")
        self.assertEqual(
            [(row["kind"], row["path"]) for row in act["transitions"]],
            [("modify", "src/ui/view.c")],
        )
        run("apply", str(act_path), "--root", str(self.root))
        source = (self.root / "src/ui/view.c").read_text()
        self.assertIn('#include "service/api.h"', source)
        self.assertIn("service_value()", source)
        run("verify", "--root", str(self.root), "--check")
        self.run_surface_gates(self.root)

    def test_c_dependency_redirect_rejects_ambiguous_replacement(self) -> None:
        shutil.copytree(REDIRECT_FIXTURE, self.root, dirs_exist_ok=True)
        (self.root / "src/service/duplicate.c").write_text(
            '#include "service/api.h"\n\n'
            "int service_value(void) { return 8; }\n"
        )
        plan_path = self.plan_path("ambiguous-redirect-plan.json")
        run(
            "plan",
            "UI-STORAGE-BOUNDARY",
            "--root",
            str(self.root),
            "--redirect",
            "raw_value=service_value",
            "--output",
            str(plan_path),
        )
        rejected = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
            expected=2,
        )
        self.assertIn(
            "replacement has no unique exact C function definition",
            rejected.stderr.decode(),
        )

    def test_c_dependency_redirect_does_not_invent_include_spelling(self) -> None:
        shutil.copytree(REDIRECT_FIXTURE, self.root, dirs_exist_ok=True)
        implementation = self.root / "src/service/api.c"
        implementation.write_text(
            '#include "storage/raw.h"\n\n'
            "int service_value(void) { return raw_value(); }\n"
        )
        plan_path = self.plan_path("unobserved-include-redirect-plan.json")
        run(
            "plan",
            "UI-STORAGE-BOUNDARY",
            "--root",
            str(self.root),
            "--redirect",
            "raw_value=service_value",
            "--output",
            str(plan_path),
        )
        rejected = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
            expected=2,
        )
        self.assertIn(
            "replacement declaration has no unique observed include spelling",
            rejected.stderr.decode(),
        )

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
        summary = run(
            "plan", "--root", str(self.root), "--output", str(plan_path)
        ).stdout.decode()
        self.assertEqual(
            summary,
            "Result: items=1; executable=1; non-executable=0; "
            "unknowns=0; preserved-constraints=0\n",
        )
        first = json.loads(plan_path.read_bytes())
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        second = json.loads(plan_path.read_bytes())
        self.assertEqual(second["source"], first["source"])

        act_path, patch = self.accepted_act(plan_path)
        self.assertEqual(patch["acceptance"]["status"], "satisfied")
        self.assertTrue(legacy.exists())
        run("apply", str(act_path), "--root", str(self.root))
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
            "--format",
            "json",
            expected=2,
        )
        self.assertFalse(completed.stdout)
        self.assertIn("Plan source", completed.stderr.decode())
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

        completed = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertFalse(completed.stdout)
        self.assertIn("manual or blocked item", completed.stderr.decode())
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

        act_path, patch = self.accepted_act(plan_path)
        self.assertEqual(patch["transitions"], [])
        self.assertEqual(patch["acceptance"]["status"], "satisfied")
        applied = run(
            "apply", str(act_path), "--root", str(self.root)
        ).stdout.decode()
        self.assertEqual(applied, "Result: applied-transitions=0\n")

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
            "--format",
            "json",
            expected=2,
        )
        self.assertFalse(completed.stdout)
        self.assertIn("failing constraint", completed.stderr.decode())
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

    def test_asserted_create_and_move_apply_as_one_accepted_act(self) -> None:
        self.configure(
            {
                "CREATE": {
                    "kind": "required_paths",
                    "paths": ["created.py"],
                    "owner": "architecture",
                    "rationale": "The generated entry exists.",
                },
                "MOVE": {
                    "kind": "required_paths",
                    "paths": ["new/name.py"],
                    "owner": "architecture",
                    "rationale": "The source has its reviewed location.",
                },
                "NO-OLD": {
                    "kind": "forbidden_paths",
                    "paths": ["old/name.py"],
                    "owner": "architecture",
                    "rationale": "The old location stays absent.",
                },
            }
        )
        old = self.root / "old/name.py"
        old.parent.mkdir()
        old.write_text("value = 1\n")
        plan_path = self.plan_path()
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        by_constraint = {
            item["origins"][0]["constraint_id"]: item for item in plan["items"]
        }
        create = by_constraint["CREATE"]
        create.update(
            provenance="asserted",
            executable=True,
            non_executable_reasons=[],
            unknowns=[],
            operation={
                "action": "create_file",
                "path": "created.py",
                "content": "created = True\n",
            },
        )
        move = by_constraint["MOVE"]
        move.update(
            provenance="asserted",
            executable=True,
            non_executable_reasons=[],
            unknowns=[],
            operation={
                "action": "move_file",
                "source_path": "old/name.py",
                "destination_path": "new/name.py",
                "source_sha256": hashlib.sha256(old.read_bytes()).hexdigest(),
            },
            acceptance={"constraints": ["MOVE", "NO-OLD"]},
        )
        plan["items"] = [create, move]
        plan["unknowns"] = []
        plan_path.write_text(json.dumps(plan, sort_keys=True))

        act_path, patch = self.accepted_act(plan_path)
        self.assertEqual(
            {transition["kind"] for transition in patch["transitions"]},
            {"create", "move"},
        )
        self.assertTrue(old.exists())
        run("apply", str(act_path), "--root", str(self.root))
        self.assertFalse(old.exists())
        self.assertEqual((self.root / "new/name.py").read_text(), "value = 1\n")
        self.assertEqual(
            (self.root / "created.py").read_text(), "created = True\n"
        )

    def test_commit_failure_restores_already_removed_files(self) -> None:
        self.configure(
            {
                "NO-LEGACY": {
                    "kind": "forbidden_paths",
                    "paths": ["a.py", "b.py"],
                    "owner": "architecture",
                    "rationale": "Both legacy files stay absent.",
                }
            }
        )
        first = self.root / "a.py"
        second = self.root / "b.py"
        first.write_text("a = 1\n")
        second.write_text("b = 1\n")
        plan_path = self.plan_path()
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        act_path, _patch = self.accepted_act(plan_path)
        original_unlink = Path.unlink

        def fail_second(path: Path, *args: object, **kwargs: object) -> None:
            if path == second:
                raise OSError("injected second delete failure")
            original_unlink(path, *args, **kwargs)

        with mock.patch.object(Path, "unlink", fail_second):
            with self.assertRaisesRegex(OSError, "injected second delete failure"):
                act_transport._commit_act(self.root, act_path.read_bytes())
        self.assertEqual(first.read_text(), "a = 1\n")
        self.assertEqual(second.read_text(), "b = 1\n")


if __name__ == "__main__":
    unittest.main()
