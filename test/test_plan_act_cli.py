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
SAMPLE_FIXTURE = ROOT / "test/fixtures/sample"
MAKE_PROVIDER = {
    "definition_sha256": (
        "a9d5a1c18d33c5c63cd34ced178608b7"
        "bb184126a4a9aeda6ad9c057c2e98fa3"
    ),
    "kind": "make_variable",
    "path": "Makefile",
    "variable": "WASM_EXPORTS",
}


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

    def configure(
        self,
        constraints: dict[str, object],
        *,
        layers: list[dict[str, object]] | None = None,
        tests: list[dict[str, object]] | None = None,
    ) -> None:
        document: dict[str, object] = {
            "project": "plan-act-fixture",
            "constraints": constraints,
        }
        if tests is not None:
            document["tests"] = tests
        if layers is not None:
            document["layers"] = layers
        (self.root / "archbird.json").write_text(
            json.dumps(document, sort_keys=True)
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
        applied = run(
            "apply", str(act_path), "--root", str(self.root)
        ).stdout.decode()
        self.assertEqual(
            applied,
            "Result: applied-transitions=1; state=applied\n",
        )
        self.assertFalse(legacy.exists())
        replay = run(
            "apply",
            str(act_path),
            "--root",
            str(self.root),
        ).stdout.decode()
        self.assertEqual(
            replay,
            "Result: applied-transitions=0; state=already-satisfied\n",
        )

    def test_package_entrypoint_plan_uses_lossless_native_json_edits(self) -> None:
        cases = (
            ("main", None, "main", "js/index.js"),
            ("exports", "js/runtime.js", "exports:.", "./js/index.js"),
        )
        for name, existing, route, expected in cases:
            with self.subTest(route=route):
                project = self.root / name
                shutil.copytree(SAMPLE_FIXTURE, project)
                package_path = project / "package.json"
                package = json.loads(package_path.read_bytes())
                if route == "main":
                    package.pop("main")
                else:
                    package["exports"]["."] = existing
                    configuration_path = project / "archbird.json"
                    configuration = json.loads(configuration_path.read_bytes())
                    configuration["constraints"]["REQUIRED-ENTRYPOINT-001"][
                        "route"
                    ] = route
                    configuration_path.write_text(
                        json.dumps(configuration, indent=2) + "\n"
                    )
                package_path.write_text(json.dumps(package, indent=2) + "\n")
                plan_path = self.plan_path(f"{name}-entrypoint-plan.json")
                run(
                    "plan",
                    "REQUIRED-ENTRYPOINT-001",
                    "--root",
                    str(project),
                    "--pretty",
                    "--output",
                    str(plan_path),
                )
                plan = json.loads(plan_path.read_bytes())
                self.assertEqual(len(plan["items"]), 1)
                self.assertEqual(
                    plan["items"][0]["operation"],
                    {
                        "action": "set_package_entrypoint",
                        "package": "npm",
                        "path": "package.json",
                        "route": route,
                        "target": "js/index.js",
                    },
                )
                self.assertTrue(plan["items"][0]["executable"])
                act_path, act = self.accepted_act(
                    plan_path, f"{name}-entrypoint-act.json", root=project
                )
                self.assertEqual(len(act["executors"]), 1)
                executor = act["executors"][0]
                self.assertEqual(
                    executor["capability"],
                    "archbird.native.json.package-entrypoint@1",
                )
                self.assertEqual(executor["item_ids"], [plan["items"][0]["id"]])
                self.assertTrue(executor["deterministic"])
                self.assertEqual(executor["matches"], 1)
                self.assertEqual(executor["reads"], ["package.json"])
                self.assertEqual(executor["writes"], ["package.json"])
                run("apply", str(act_path), "--root", str(project))
                changed = json.loads(package_path.read_bytes())
                if route == "main":
                    self.assertEqual(changed["main"], expected)
                else:
                    self.assertEqual(changed["exports"]["."], expected)
                run("verify", "--root", str(project), "--check")

    def test_conditional_package_export_remains_manual(self) -> None:
        project = self.root / "conditional-export"
        shutil.copytree(SAMPLE_FIXTURE, project)
        package_path = project / "package.json"
        package = json.loads(package_path.read_bytes())
        package["exports"]["."] = {
            "import": "./js/runtime.js",
            "require": "./js/runtime.js",
        }
        package_path.write_text(json.dumps(package, indent=2) + "\n")
        configuration_path = project / "archbird.json"
        configuration = json.loads(configuration_path.read_bytes())
        configuration["constraints"]["REQUIRED-ENTRYPOINT-001"][
            "route"
        ] = "exports:."
        configuration_path.write_text(json.dumps(configuration, indent=2) + "\n")
        plan_path = self.plan_path("conditional-entrypoint-plan.json")
        run(
            "plan",
            "REQUIRED-ENTRYPOINT-001",
            "--root",
            str(project),
            "--pretty",
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["items"][0]["operation"]["action"], "manual")
        self.assertFalse(plan["items"][0]["executable"])
        self.assertIn(
            "nested or conditional",
            plan["items"][0]["non_executable_reasons"][0],
        )

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
        duplicate_path = self.plan_path("duplicate-token.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--rename",
            "core_add=core_sum",
            "--output",
            str(duplicate_path),
        )
        duplicate_plan = json.loads(duplicate_path.read_bytes())
        self.assertNotIn("source_sha256", duplicate_plan["items"][0]["operation"])
        duplicate = run(
            "act",
            str(duplicate_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "Make variable token edit expected one match but found 2",
            duplicate.stderr.decode(),
        )
        makefile.write_bytes(original)

        makefile.write_bytes(
            original.replace(b"_core_add", b"_core_add _core_sum")
        )
        already_present_path = self.plan_path("target-already-present.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--rename",
            "core_add=core_sum",
            "--output",
            str(already_present_path),
        )
        already_present = json.loads(already_present_path.read_bytes())
        self.assertEqual(
            already_present["items"][0]["operation"],
            {
                "action": "remove_provider_capability",
                "capability": "core_add",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
            },
        )
        already_patch = run(
            "act",
            str(already_present_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn("-WASM_EXPORTS = _core_add _core_sum", already_patch)
        self.assertIn("+WASM_EXPORTS = _core_sum", already_patch)
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
                "action": "rename_provider_capability",
                "from": "core_add",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
                "to": "core_sum",
            },
        )
        tampered = json.loads(json.dumps(plan))
        tampered["items"][0]["operation"]["provider"]["variable"] = (
            "OTHER_EXPORTS"
        )
        tampered_path = self.plan_path("tampered-provider-plan.json")
        tampered_path.write_text(json.dumps(tampered, sort_keys=True))
        rejected = run(
            "act",
            str(tampered_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "configured provider differs from the current Map",
            rejected.stderr.decode(),
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
        self.assertEqual(plan["schema_version"], 4)
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
            item["operation"],
            {
                "action": "rename_provider_capability",
                "from": "core_add",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
                "to": "core_sum",
            },
        )

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
            plan["items"][0]["operation"],
            {
                "action": "rename_provider_capability",
                "from": "core_add",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
                "to": "core_sum",
            },
        )

        act_path, _ = self.accepted_act(
            plan_path, "git-residual-act.json", root=project
        )
        run("apply", str(act_path), "--root", str(project))
        run("verify", "FFI-SURFACE", "--root", str(project), "--check")
        self.run_surface_gates(project)

    def test_required_provider_surface_derives_exact_make_insertion(self) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        configuration["bridges"][0]["providers"].append(
            {
                "kind": "file_pattern",
                "path": "src/core.h",
                "pattern": r"\b(core_[A-Za-z0-9_]+)\s*\(",
            }
        )
        configuration["constraints"]["FFI-SURFACE"][
            "require_all_providers"
        ] = True
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
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
            "provider capability is absent from 1 of 2 configured providers: "
            "core_sum",
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
        self.assertEqual(len(item["origins"]), 1)
        self.assertEqual(
            item["operation"],
            {
                "action": "add_provider_capability",
                "capability": "core_sum",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
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

    def test_missing_napi_export_is_grounded_only_from_exact_c_evidence(
        self,
    ) -> None:
        shutil.copytree(SAMPLE_FIXTURE, self.root, dirs_exist_ok=True)
        addon = self.root / "native/addon.c"
        missing_registration = addon.read_bytes().replace(
            b'  {"core_mul", NULL, napi_core_mul},\n', b""
        )
        self.assertNotEqual(missing_registration, addon.read_bytes())
        addon.write_bytes(missing_registration)
        failed = run(
            "verify",
            "PROVIDER-SURFACE-JS-BINDING",
            "--root",
            str(self.root),
            "--check",
            expected=1,
        )
        self.assertIn("core_mul", failed.stdout.decode())

        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        bridge = next(
            row for row in configuration["bridges"]
            if row["name"] == "js-binding"
        )
        bridge["providers"][0]["path"] = "native/**"
        (self.root / "native/extra.c").write_text(
            "static int unrelated(void) { return 0; }\n"
        )
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
        glob_plan_path = self.plan_path("napi-glob-plan.json")
        run(
            "plan",
            "PROVIDER-SURFACE-JS-BINDING",
            "--root",
            str(self.root),
            "--output",
            str(glob_plan_path),
        )
        glob_plan = json.loads(glob_plan_path.read_bytes())
        self.assertTrue(glob_plan["items"])
        self.assertTrue(
            all(not item["executable"] for item in glob_plan["items"])
        )
        self.assertNotIn(
            "add_provider_capability",
            {item["operation"]["action"] for item in glob_plan["items"]},
        )

        bridge["providers"][0]["path"] = "native/addon.c"
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
        wrapper = b"static napi_value napi_core_mul(void) { return 0; }\n"
        self.assertIn(wrapper, missing_registration)
        addon.write_bytes(missing_registration.replace(wrapper, b""))
        wrapper_plan_path = self.plan_path("napi-missing-wrapper-plan.json")
        run(
            "plan",
            "PROVIDER-SURFACE-JS-BINDING",
            "--root",
            str(self.root),
            "--output",
            str(wrapper_plan_path),
        )
        wrapper_plan = json.loads(wrapper_plan_path.read_bytes())
        self.assertTrue(wrapper_plan["items"])
        self.assertTrue(
            all(not item["executable"] for item in wrapper_plan["items"])
        )
        self.assertNotIn(
            "add_provider_capability",
            {item["operation"]["action"] for item in wrapper_plan["items"]},
        )

        addon.write_bytes(missing_registration)
        plan_path = self.plan_path("napi-export-plan.json")
        run(
            "plan",
            "PROVIDER-SURFACE-JS-BINDING",
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
        operation = item["operation"]
        self.assertEqual(
            {
                key: value
                for key, value in operation.items()
                if key != "provider"
            },
            {
                "action": "add_provider_capability",
                "capability": "core_mul",
                "source_paths": ["native/addon.c"],
                "surface": "js-binding",
            },
        )
        self.assertEqual(operation["provider"]["kind"], "exports")
        self.assertEqual(operation["provider"]["path"], "native/addon.c")
        self.assertRegex(
            operation["provider"]["definition_sha256"], r"^[0-9a-f]{64}$"
        )

        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn(
            '+  {"core_mul", NULL, napi_core_mul},',
            preview,
        )
        self.assertEqual(addon.read_bytes(), missing_registration)

        act_path, act = self.accepted_act(plan_path, "napi-export-act.json")
        self.assertEqual(
            act["executors"],
            [
                {
                    "capability": "archbird.native.c.napi-export@1",
                    "deterministic": True,
                    "implementation_sha256": (
                        act["tool"]["implementation_sha256"]
                    ),
                    "item_ids": [item["id"]],
                    "matches": 1,
                    "reads": ["native/addon.c"],
                    "skipped": 0,
                    "unsupported": 0,
                    "writes": ["native/addon.c"],
                }
            ],
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn(
            b'  {"core_mul", NULL, napi_core_mul},\n',
            addon.read_bytes(),
        )
        run(
            "verify",
            "PROVIDER-SURFACE-JS-BINDING",
            "--root",
            str(self.root),
            "--check",
        )
        compiler = shutil.which("cc")
        if compiler is not None:
            compiled = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsyntax-only",
                    str(addon),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(
                compiled.returncode, 0, compiled.stdout.decode()
            )

    def test_napi_macro_export_preserves_multiline_crlf_layout(self) -> None:
        shutil.copytree(SAMPLE_FIXTURE, self.root, dirs_exist_ok=True)
        addon = self.root / "native/addon.c"
        addon.write_bytes(
            (
                "#include <stddef.h>\r\n"
                "\r\n"
                "typedef int napi_value;\r\n"
                "typedef napi_value (*napi_callback)(void);\r\n"
                "typedef struct {\r\n"
                "  const char *name;\r\n"
                "  void *data;\r\n"
                "  napi_callback method;\r\n"
                "} napi_property_descriptor;\r\n"
                "\r\n"
                "#define DECLARE_NAPI_METHOD(name, method) "
                "{name, NULL, method}\r\n"
                "\r\n"
                "static napi_value napi_core_add(void) { return 0; }\r\n"
                "static napi_value napi_core_mul(void) { return 0; }\r\n"
                "\r\n"
                "static const napi_property_descriptor props[] = {\r\n"
                "  DECLARE_NAPI_METHOD(\r\n"
                '    "core_add",\r\n'
                "    napi_core_add\r\n"
                "  ),\r\n"
                "};\r\n"
            ).encode()
        )
        plan_path = self.plan_path("napi-macro-plan.json")
        run(
            "plan",
            "PROVIDER-SURFACE-JS-BINDING",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 1)
        self.assertTrue(plan["items"][0]["executable"])
        act_path, _ = self.accepted_act(plan_path, "napi-macro-act.json")
        run("apply", str(act_path), "--root", str(self.root))
        changed = addon.read_bytes()
        self.assertNotIn(b"\n", changed.replace(b"\r\n", b""))
        self.assertIn(
            (
                "  DECLARE_NAPI_METHOD(\r\n"
                '    "core_mul",\r\n'
                "    napi_core_mul\r\n"
                "  ),\r\n"
            ).encode(),
            changed,
        )
        run(
            "verify",
            "PROVIDER-SURFACE-JS-BINDING",
            "--root",
            str(self.root),
            "--check",
        )

    def test_provider_surface_union_remains_default(self) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        configuration["bridges"][0]["providers"].append(
            {
                "kind": "file_pattern",
                "path": "src/core.h",
                "pattern": r"\b(core_[A-Za-z0-9_]+)\s*\(",
            }
        )
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )

        run("verify", "FFI-SURFACE", "--root", str(self.root), "--check")

    def test_provider_parity_plans_c_declaration_before_make_registration(
        self,
    ) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        configuration["bridges"][0]["providers"].append(
            {
                "kind": "file_pattern",
                "path": "src/core.h",
                "pattern": r"\b(core_[A-Za-z0-9_]+)\s*\(",
            }
        )
        configuration["constraints"]["FFI-SURFACE"][
            "require_all_providers"
        ] = True
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
        header = self.root / "src/core.h"
        header.write_text(
            header.read_text().replace(
                "int core_sum(int left, int right);\n", ""
            )
        )

        plan_path = self.plan_path("provider-parity-plan.json")
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
        by_kind = {
            item["operation"]["provider"]["kind"]: item
            for item in plan["items"]
        }
        self.assertEqual(set(by_kind), {"file_pattern", "make_variable"})
        c_item = by_kind["file_pattern"]
        make_item = by_kind["make_variable"]
        self.assertEqual(c_item["depends_on"], [])
        self.assertEqual(make_item["depends_on"], [c_item["id"]])
        self.assertEqual(
            c_item["operation"],
            {
                "action": "add_provider_capability",
                "capability": "core_sum",
                "provider": {
                    "definition_sha256": (
                        "1b929f6f0a328ebc5f8c3fc497ade7ec"
                        "b6722440ad7af15fe455c27bf5133e40"
                    ),
                    "kind": "file_pattern",
                    "path": "src/core.h",
                },
                "source_paths": ["src/core.c", "src/core.h"],
                "surface": "ffi",
            },
        )
        self.assertEqual(make_item["operation"]["provider"], MAKE_PROVIDER)

        act_path, act = self.accepted_act(
            plan_path, "provider-parity-act.json"
        )
        self.assertEqual(len(act["transitions"]), 2)
        self.assertEqual(
            {row["capability"] for row in act["executors"]},
            {
                "archbird.native.c.provider-capability@1",
                "archbird.native.make.provider-capability@1",
            },
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn(
            "int core_sum(int left, int right);", header.read_text()
        )
        self.assertIn(
            "WASM_EXPORTS = _core_peer _core_sum",
            (self.root / "Makefile").read_text(),
        )
        run("verify", "FFI-SURFACE", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_cross_constraint_plan_composes_shared_declaration_effect(
        self,
    ) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        configuration["bridges"][0]["providers"].append(
            {
                "kind": "file_pattern",
                "path": "src/core.h",
                "pattern": r"\b(core_[A-Za-z0-9_]+)\s*\(",
            }
        )
        configuration["constraints"]["FFI-SURFACE"][
            "require_all_providers"
        ] = True
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
        header = self.root / "src/core.h"
        header.write_text(
            header.read_text().replace(
                "int core_sum(int left, int right);\n", ""
            )
        )

        plan_path = self.plan_path("cross-constraint-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(len(plan["items"]), 3)
        declaration = next(
            item
            for item in plan["items"]
            if item["operation"]["action"] == "declare_symbol"
        )
        provider_items = [
            item
            for item in plan["items"]
            if item["operation"]["action"] == "add_provider_capability"
        ]
        file_provider = next(
            item
            for item in provider_items
            if item["operation"]["provider"]["kind"] == "file_pattern"
        )
        make_provider = next(
            item
            for item in provider_items
            if item["operation"]["provider"]["kind"] == "make_variable"
        )
        self.assertEqual(file_provider["depends_on"], [declaration["id"]])
        self.assertEqual(
            make_provider["depends_on"],
            sorted([declaration["id"], file_provider["id"]]),
        )

        act_path, act = self.accepted_act(
            plan_path, "cross-constraint-act.json"
        )
        header_transition = next(
            row for row in act["transitions"] if row["path"] == "src/core.h"
        )
        self.assertEqual(
            header_transition["item_ids"],
            sorted([declaration["id"], file_provider["id"]]),
        )
        self.assertEqual(len(act["transitions"]), 2)
        run("apply", str(act_path), "--root", str(self.root))
        self.assertEqual(
            header.read_text().count("int core_sum(int left, int right);"),
            1,
        )
        run("verify", "--root", str(self.root), "--check")
        self.run_surface_gates()

    def test_distinct_providers_sharing_one_edit_target_remain_manual(
        self,
    ) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        configuration["bridges"][0]["providers"].extend(
            [
                {
                    "kind": "file_pattern",
                    "path": "src/core.h",
                    "pattern": r"\b(core_[A-Za-z0-9_]+)\s*\(",
                },
                {
                    "kind": "file_pattern",
                    "path": "src/core.h",
                    "pattern": r"\b(core_(?:peer|sum))\s*\(",
                },
            ]
        )
        configuration["constraints"]["FFI-SURFACE"][
            "require_all_providers"
        ] = True
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
        makefile = self.root / "Makefile"
        makefile.write_text(
            makefile.read_text().replace(
                "WASM_EXPORTS = _core_peer",
                "WASM_EXPORTS = _core_peer _core_sum",
            )
        )
        header = self.root / "src/core.h"
        header.write_text(
            header.read_text().replace(
                "int core_sum(int left, int right);\n", ""
            )
        )

        mapped = json.loads(
            run(
                "map",
                "--root",
                str(self.root),
                "--format",
                "json",
            ).stdout
        )
        file_providers = [
            provider
            for provider in mapped["surfaces"][0]["providers"]
            if provider["source"] == "file-pattern"
        ]
        self.assertEqual(len(file_providers), 2)
        self.assertEqual(
            len({provider["definition_sha256"] for provider in file_providers}),
            2,
        )

        plan_path = self.plan_path("shared-provider-target-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertTrue(plan["items"])
        self.assertTrue(all(not item["executable"] for item in plan["items"]))
        self.assertNotIn(
            "add_provider_capability",
            {item["operation"]["action"] for item in plan["items"]},
        )

    def test_file_provider_without_native_executor_remains_manual(self) -> None:
        shutil.copytree(REGISTRATION_FIXTURE, self.root, dirs_exist_ok=True)
        configuration_path = self.root / "archbird.json"
        configuration = json.loads(configuration_path.read_text())
        configuration["bridges"][0]["providers"].append(
            {
                "kind": "file_pattern",
                "path": "provider.txt",
                "pattern": r"\b(core_[A-Za-z0-9_]+)\s*\(",
            }
        )
        configuration["constraints"]["FFI-SURFACE"][
            "require_all_providers"
        ] = True
        configuration_path.write_text(
            json.dumps(configuration, sort_keys=True, separators=(",", ":"))
        )
        (self.root / "provider.txt").write_text("core_peer();\n")
        makefile = self.root / "Makefile"
        makefile.write_text(
            makefile.read_text().replace(
                "WASM_EXPORTS = _core_peer",
                "WASM_EXPORTS = _core_peer _core_sum",
            )
        )

        plan_path = self.plan_path("unsupported-provider-plan.json")
        run(
            "plan",
            "FFI-SURFACE",
            "--root",
            str(self.root),
            "--output",
            str(plan_path),
        )
        plan = json.loads(plan_path.read_bytes())
        self.assertTrue(plan["items"])
        self.assertTrue(all(not item["executable"] for item in plan["items"]))
        self.assertNotIn(
            "add_provider_capability",
            {item["operation"]["action"] for item in plan["items"]},
        )

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
        self.assertEqual(
            operations["declare_symbol"],
            {
                "action": "declare_symbol",
                "path": "src/core.h",
                "symbol": "core_sum",
                "source_paths": ["src/core.c", "src/core.h"],
            },
        )
        tampered = json.loads(json.dumps(plan))
        declaration_item = next(
            item
            for item in tampered["items"]
            if item["operation"]["action"] == "declare_symbol"
        )
        declaration_item["operation"]["source_paths"] = [
            "js/decoy.js",
            "src/core.h",
        ]
        tampered_path = self.plan_path("tampered-source-closure-plan.json")
        tampered_path.write_text(json.dumps(tampered, sort_keys=True))
        rejected = run(
            "act",
            str(tampered_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "declared source closure differs from the Map proof",
            rejected.stderr.decode(),
        )
        registration = operations["add_provider_capability"]
        self.assertEqual(
            registration,
            {
                "action": "add_provider_capability",
                "capability": "core_sum",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
            },
        )
        declaration_item = next(
            item
            for item in plan["items"]
            if item["operation"]["action"] == "declare_symbol"
        )
        registration_item = next(
            item
            for item in plan["items"]
            if item["operation"]["action"] == "add_provider_capability"
        )
        self.assertEqual(
            registration_item["depends_on"], [declaration_item["id"]]
        )
        self.assertEqual(declaration_item["depends_on"], [])

        act_path, patch = self.accepted_act(
            plan_path, "coordinated-surface-act.json"
        )
        self.assertEqual(len(patch["transitions"]), 2)
        executors = {
            row["capability"]: row for row in patch["executors"]
        }
        self.assertEqual(
            set(executors),
            {
                "archbird.native.c.declare-symbol@1",
                "archbird.native.make.provider-capability@1",
            },
        )
        self.assertEqual(
            executors["archbird.native.c.declare-symbol@1"],
            {
                "capability": "archbird.native.c.declare-symbol@1",
                "deterministic": True,
                "implementation_sha256": patch["tool"][
                    "implementation_sha256"
                ],
                "item_ids": [declaration_item["id"]],
                "matches": 1,
                "reads": ["src/core.c", "src/core.h"],
                "skipped": 0,
                "unsupported": 0,
                "writes": ["src/core.h"],
            },
        )
        self.assertEqual(
            executors["archbird.native.make.provider-capability@1"],
            {
                "capability": (
                    "archbird.native.make.provider-capability@1"
                ),
                "deterministic": True,
                "implementation_sha256": patch["tool"][
                    "implementation_sha256"
                ],
                "item_ids": [registration_item["id"]],
                "matches": 1,
                "reads": ["Makefile"],
                "skipped": 0,
                "unsupported": 0,
                "writes": ["Makefile"],
            },
        )
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
        self.assertTrue(manual["items"][0]["executable"])
        self.assertEqual(
            manual["items"][0]["operation"]["action"], "declare_symbol"
        )
        style_rejected = run(
            "act",
            str(manual_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "signature style",
            style_rejected.stderr.decode(),
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
            "does not satisfy the Plan contract",
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
        self.assertTrue(static_plan["items"][0]["executable"])
        static_rejected = run(
            "act",
            str(static_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "safe external declaration",
            static_rejected.stderr.decode(),
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
                item["operation"]["capability"]
                for item in plan["items"]
            },
            {"core_product", "core_sum"},
        )
        self.assertEqual(
            {item["operation"]["action"] for item in plan["items"]},
            {"add_provider_capability"},
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
                "action": "remove_provider_capability",
                "capability": "core_add",
                "provider": MAKE_PROVIDER,
                "surface": "ffi",
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
        (self.root / "unrelated.py").write_text(
            "def old_api(value):\n    return value - 1\n"
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
        self.assertEqual(item["operation"]["projection"]["paths"], ["api.py"])
        self.assertEqual(
            item["operation"]["source_paths"],
            ["api.py", "consumer.py"],
        )
        self.assertEqual(
            item["operation"]["projection_id"], "plan-symbol-occurrences"
        )
        self.assertRegex(
            item["operation"]["projection_content_sha256"], r"^[0-9a-f]{64}$"
        )
        self.assertNotIn("sites", item["operation"])
        self.assertNotIn("coverage", item["operation"])
        self.assertNotIn("projection_result_sha256", item["operation"])

        narrowed = json.loads(json.dumps(plan_document))
        narrowed["items"][0]["operation"]["source_paths"] = ["api.py"]
        narrowed_path = self.plan_path("narrowed-rename-plan.json")
        narrowed_path.write_text(json.dumps(narrowed, sort_keys=True))
        rejected = run(
            "act",
            str(narrowed_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "Plan source scope differs from the current projection",
            rejected.stderr.decode(),
        )

        stale_projection = json.loads(json.dumps(plan_document))
        stale_projection["items"][0]["operation"][
            "projection_content_sha256"
        ] = "0" * 64
        stale_path = self.plan_path("stale-rename-projection-plan.json")
        stale_path.write_text(json.dumps(stale_projection, sort_keys=True))
        rejected = run(
            "act",
            str(stale_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "occurrence projection differs from the Plan",
            rejected.stderr.decode(),
        )

        legacy_grounding = json.loads(json.dumps(plan_document))
        legacy_grounding["items"][0]["operation"]["sites"] = []
        legacy_path = self.plan_path("legacy-grounded-rename-plan.json")
        legacy_path.write_text(json.dumps(legacy_grounding, sort_keys=True))
        rejected = run(
            "act",
            str(legacy_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "document does not satisfy the Plan contract",
            rejected.stderr.decode(),
        )

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
        act_path, rename_act = self.accepted_act(
            plan_path, "rename-act.json"
        )
        self.assertEqual(
            rename_act["executors"],
            [
                {
                    "capability": (
                        "archbird.native.python.rename-symbol@1"
                    ),
                    "deterministic": True,
                    "implementation_sha256": rename_act["tool"][
                        "implementation_sha256"
                    ],
                    "item_ids": [item["id"]],
                    "matches": 3,
                    "reads": ["api.py", "consumer.py"],
                    "skipped": 0,
                    "unsupported": 0,
                    "writes": ["api.py", "consumer.py"],
                }
            ],
        )
        self.assertIn("old_api", (self.root / "api.py").read_text())
        run("apply", str(act_path), "--root", str(self.root))
        self.assertNotIn("old_api", (self.root / "api.py").read_text())
        self.assertNotIn("old_api", (self.root / "consumer.py").read_text())
        self.assertIn("old_api", (self.root / "unrelated.py").read_text())
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

    def test_act_rejects_rename_without_a_language_executor(self) -> None:
        (self.root / "api.c").write_text(
            "int old_api(void) { return 1; }\n"
        )
        (self.root / "archbird.json").write_text(
            json.dumps(
                {
                    "project": "plan-act-c-rename",
                    "layers": [
                        {
                            "name": "c",
                            "language": "c",
                            "globs": ["*.c"],
                        }
                    ],
                    "projections": {
                        "api-symbols": {
                            "select": "symbols",
                            "paths": ["api.c"],
                        }
                    },
                    "constraints": {
                        "API-SURFACE": {
                            "assert": "set_equal",
                            "actual": {"projection": "api-symbols"},
                            "expected": {"literal": ["new_api"]},
                            "owner": "architecture",
                            "rationale": "The reviewed C API rename is complete.",
                        }
                    },
                },
                sort_keys=True,
            )
        )
        plan_path = self.plan_path("unsupported-c-rename-plan.json")
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
        operation = json.loads(plan_path.read_bytes())["items"][0]["operation"]
        self.assertEqual(operation["action"], "rename_symbol")
        self.assertNotIn("sites", operation)
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
            "no language executor supports the mapped source language",
            rejected.stderr.decode(),
        )
        self.assertIn("old_api", (self.root / "api.c").read_text())

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
        self.assertEqual(
            act["executors"][0]["capability"],
            "archbird.native.c.redirect-dependency@1",
        )
        self.assertEqual(act["executors"][0]["matches"], 2)
        self.assertEqual(act["executors"][0]["reads"], ["src/ui/view.c"])
        self.assertEqual(act["executors"][0]["writes"], ["src/ui/view.c"])
        run("apply", str(act_path), "--root", str(self.root))
        source = (self.root / "src/ui/view.c").read_text()
        self.assertIn('#include "service/api.h"', source)
        self.assertIn("service_value()", source)
        run("verify", "--root", str(self.root), "--check")
        self.run_surface_gates(self.root)

    def configure_python_dependency_redirect(
        self, *, observed_replacement: bool = True, multiple_imports: bool = False
    ) -> None:
        for directory in ("app", "service", "storage"):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        imported = "raw_value, other" if multiple_imports else "raw_value"
        (self.root / "app/main.py").write_text(
            f"from storage.raw import {imported}\n\n"
            "def render():\n"
            "    return raw_value()\n"
        )
        (self.root / "app/alias.py").write_text(
            "from storage.raw import raw_value as read\n\n"
            "def alias_render():\n"
            "    return read()\n"
        )
        if observed_replacement:
            (self.root / "app/peer.py").write_text(
                "from service.api import service_value\n\n"
                "def peer():\n"
                "    return service_value()\n"
            )
        (self.root / "service/api.py").write_text(
            "from storage.raw import raw_value\n\n"
            "def service_value():\n"
            "    return raw_value()\n"
        )
        (self.root / "storage/raw.py").write_text(
            "def raw_value():\n"
            "    return 7\n\n"
            "def other():\n"
            "    return 8\n"
        )
        (self.root / "archbird.json").write_text(
            json.dumps(
                {
                    "project": "python-dependency-redirect",
                    "layers": [
                        {
                            "name": "python",
                            "language": "python",
                            "globs": ["**/*.py"],
                            "import_roots": ["."],
                        }
                    ],
                    "components": [
                        {"name": "app", "paths": ["app/**"]},
                        {"name": "service", "paths": ["service/**"]},
                        {"name": "storage", "paths": ["storage/**"]},
                    ],
                    "constraints": {
                        "APP-STORAGE-BOUNDARY": {
                            "kind": "forbidden_component_edges",
                            "edges": [
                                {
                                    "source": "app",
                                    "kind": "import",
                                    "target": "storage",
                                }
                            ],
                            "kinds": ["import"],
                            "owner": "architecture",
                            "rationale": (
                                "Application code reaches storage through "
                                "the service boundary."
                            ),
                        }
                    },
                },
                sort_keys=True,
            )
        )

    def python_dependency_redirect_plan(self, name: str) -> Path:
        plan_path = self.plan_path(name)
        run(
            "plan",
            "APP-STORAGE-BOUNDARY",
            "--root",
            str(self.root),
            "--redirect",
            "raw_value=service_value",
            "--output",
            str(plan_path),
        )
        return plan_path

    def test_python_dependency_redirect_preserves_aliases_and_closes_edge(
        self,
    ) -> None:
        self.configure_python_dependency_redirect()
        plan_path = self.python_dependency_redirect_plan(
            "python-dependency-redirect-plan.json"
        )
        plan = json.loads(plan_path.read_bytes())
        operation = plan["items"][0]["operation"]
        self.assertEqual(operation["action"], "redirect_dependency")
        self.assertEqual(
            operation["source_paths"], ["app/alias.py", "app/main.py"]
        )
        widened = json.loads(json.dumps(plan))
        widened["items"][0]["operation"]["source_paths"].append("app/peer.py")
        widened_path = self.plan_path("python-widened-redirect-plan.json")
        widened_path.write_text(json.dumps(widened, sort_keys=True))
        rejected = run(
            "act",
            str(widened_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
            expected=2,
        )
        self.assertIn(
            "Plan source scope differs from the current relation",
            rejected.stderr.decode(),
        )
        preview = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "patch",
        ).stdout.decode()
        self.assertIn(
            "-from storage.raw import raw_value as read", preview
        )
        self.assertIn(
            "+from service.api import service_value as read", preview
        )
        self.assertIn("-from storage.raw import raw_value", preview)
        self.assertIn("+from service.api import service_value", preview)
        self.assertIn("-    return raw_value()", preview)
        self.assertIn("+    return service_value()", preview)
        self.assertNotIn("-    return read()", preview)

        act_path, act = self.accepted_act(
            plan_path, "python-dependency-redirect-act.json"
        )
        self.assertEqual(
            [row["path"] for row in act["transitions"]],
            ["app/alias.py", "app/main.py"],
        )
        self.assertEqual(
            act["executors"][0]["capability"],
            "archbird.native.python.redirect-dependency@1",
        )
        self.assertEqual(act["executors"][0]["matches"], 5)
        self.assertEqual(
            act["executors"][0]["reads"], ["app/alias.py", "app/main.py"]
        )
        self.assertEqual(
            act["executors"][0]["writes"], ["app/alias.py", "app/main.py"]
        )
        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn(
            "from service.api import service_value as read",
            (self.root / "app/alias.py").read_text(),
        )
        self.assertIn(
            "return read()", (self.root / "app/alias.py").read_text()
        )
        self.assertIn(
            "return service_value()", (self.root / "app/main.py").read_text()
        )
        run("verify", "--root", str(self.root), "--check")
        completed = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "from app.main import render; "
                    "from app.alias import alias_render; "
                    "assert render() == alias_render() == 7"
                ),
            ],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout.decode())

    def test_python_dependency_redirect_requires_observed_module_spelling(
        self,
    ) -> None:
        self.configure_python_dependency_redirect(observed_replacement=False)
        plan_path = self.python_dependency_redirect_plan(
            "python-unobserved-module-plan.json"
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
            "replacement definition has no unique observed Python import module",
            rejected.stderr.decode(),
        )

    def test_python_dependency_redirect_rejects_multi_name_import(self) -> None:
        self.configure_python_dependency_redirect(multiple_imports=True)
        plan_path = self.python_dependency_redirect_plan(
            "python-multi-import-plan.json"
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
            "Python import does not bind exactly the redirected symbol",
            rejected.stderr.decode(),
        )

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

    def test_missing_api_and_test_route_form_a_neutral_plan_dag(self) -> None:
        self.configure(
            {
                "IMPLEMENT-API": {
                    "kind": "required_symbols",
                    "symbols": ["future_api"],
                    "paths": ["src/api.c"],
                    "kinds": ["function"],
                    "owner": "architecture",
                    "rationale": "The implementation exists.",
                },
                "DECLARE-API": {
                    "kind": "required_symbols",
                    "symbols": ["future_api"],
                    "paths": ["include/api.h"],
                    "kinds": ["declaration"],
                    "owner": "architecture",
                    "rationale": "The public declaration exists.",
                },
                "TEST-API": {
                    "kind": "required_test_route",
                    "group": "c",
                    "target": "src/api.c",
                    "selectors": ["api"],
                    "owner": "architecture",
                    "rationale": "The implementation has a test route.",
                },
            }
        )
        (self.root / "src").mkdir()
        (self.root / "include").mkdir()
        (self.root / "src/api.c").write_text(
            "int current_api(void) { return 1; }\n"
        )
        (self.root / "include/api.h").write_text(
            "int current_api(void);\n"
        )

        plan_path = self.plan_path("neutral-api-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        self.assertEqual(plan["schema_version"], 4)
        self.assertEqual(len(plan["items"]), 3)
        self.assertTrue(all(not item["executable"] for item in plan["items"]))

        by_constraint = {
            item["acceptance"]["constraints"][0]: item
            for item in plan["items"]
        }
        implementation = by_constraint["IMPLEMENT-API"]
        declaration = by_constraint["DECLARE-API"]
        test_route = by_constraint["TEST-API"]
        self.assertEqual(
            implementation["operation"],
            {
                "action": "add_symbol",
                "kinds": ["function"],
                "path": "src/api.c",
                "symbol": "future_api",
            },
        )
        self.assertEqual(
            declaration["operation"],
            {
                "action": "add_symbol",
                "kinds": ["declaration"],
                "path": "include/api.h",
                "symbol": "future_api",
            },
        )
        self.assertEqual(
            test_route["operation"],
            {
                "action": "add_test_route",
                "group": "c",
                "selectors": ["api"],
                "target": "src/api.c",
            },
        )
        self.assertEqual(implementation["depends_on"], [])
        self.assertEqual(declaration["depends_on"], [implementation["id"]])
        self.assertEqual(test_route["depends_on"], [implementation["id"]])

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

    def test_reviewed_api_and_test_submissions_complete_one_plan_dag(
        self,
    ) -> None:
        self.configure(
            {
                "IMPLEMENT-API": {
                    "kind": "required_symbols",
                    "symbols": ["future_api"],
                    "paths": ["src/api.c"],
                    "kinds": ["function"],
                    "owner": "architecture",
                    "rationale": "The implementation exists.",
                },
                "DECLARE-API": {
                    "kind": "required_symbols",
                    "symbols": ["future_api"],
                    "paths": ["include/api.h"],
                    "kinds": ["declaration"],
                    "owner": "architecture",
                    "rationale": "The public declaration exists.",
                },
                "TEST-API": {
                    "kind": "required_test_route",
                    "group": "c",
                    "target": "src/api.c",
                    "selectors": ["test_future"],
                    "owner": "architecture",
                    "rationale": "The implementation has a focused test.",
                },
            },
            layers=[
                {
                    "name": "core",
                    "language": "c",
                    "globs": ["src/**/*.c"],
                    "import_roots": ["include"],
                },
                {
                    "name": "api",
                    "language": "c",
                    "globs": ["include/**/*.h"],
                    "import_roots": ["include"],
                },
                {
                    "name": "native-tests",
                    "language": "c",
                    "globs": ["test/**/*.c"],
                    "import_roots": ["include"],
                },
            ],
            tests=[
                {
                    "name": "c",
                    "language": "c",
                    "globs": ["test/test_*.c"],
                    "route_to": ["core"],
                }
            ],
        )
        (self.root / "src").mkdir()
        (self.root / "include").mkdir()
        (self.root / "test").mkdir()
        implementation = self.root / "src/api.c"
        declaration = self.root / "include/api.h"
        test_source = self.root / "test/test_api.c"
        original_implementation = (
            '#include "api.h"\n\n'
            "int current_api(void) { return 1; }\n"
        )
        original_declaration = "int current_api(void);\n"
        original_test = (
            '#include "api.h"\n\n'
            "int test_current(void) { return current_api() != 1; }\n"
        )
        implementation.write_text(original_implementation)
        declaration.write_text(original_declaration)
        test_source.write_text(original_test)

        plan_path = self.plan_path("reviewed-api-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan_bytes = plan_path.read_bytes()
        plan = json.loads(plan_bytes)
        by_constraint = {
            item["acceptance"]["constraints"][0]: item
            for item in plan["items"]
        }
        implementation_item = by_constraint["IMPLEMENT-API"]
        declaration_item = by_constraint["DECLARE-API"]
        test_item = by_constraint["TEST-API"]
        self.assertEqual(
            declaration_item["depends_on"], [implementation_item["id"]]
        )
        self.assertEqual(
            test_item["depends_on"], [implementation_item["id"]]
        )
        self.assertEqual(
            test_item["operation"]["path"], "test/test_api.c"
        )
        markdown = run(
            "plan",
            "--root",
            str(self.root),
            "--format",
            "markdown",
        ).stdout.decode()
        self.assertIn("# Change Plan: plan-act-fixture", markdown)
        self.assertIn("### 1.", markdown)
        self.assertIn("- Depends on:", markdown)
        self.assertIn("- Acceptance:", markdown)
        self.assertIn(
            "Result: items=3; executable=0; input-required=3; unknowns=3;",
            markdown,
        )

        completed_implementation = (
            original_implementation
            + "\nint future_api(void) { return 2; }\n"
        )
        completed_declaration = (
            original_declaration + "int future_api(void);\n"
        )
        completed_test = (
            original_test
            + "\nint test_future(void) { return future_api() != 2; }\n"
        )
        implementation_input = self.plan_path("api.c")
        declaration_input = self.plan_path("api.h")
        test_input = self.plan_path("test_api.c")
        implementation_input.write_text(completed_implementation)
        declaration_input.write_text(completed_declaration)
        test_input.write_text(completed_test)

        act_path = self.plan_path("reviewed-api-act.json")
        run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{implementation_item['id']}={implementation_input}",
            "--submit",
            f"{declaration_item['id']}={declaration_input}",
            "--submit",
            f"{test_item['id']}={test_input}",
            "--format",
            "json",
            "--output",
            str(act_path),
        )
        act = json.loads(act_path.read_bytes())
        self.assertEqual(act["acceptance"]["status"], "satisfied")
        self.assertEqual(len(act["executors"]), 3)
        self.assertEqual(
            {row["capability"] for row in act["executors"]},
            {"archbird.asserted.source.replace-file@1"},
        )
        self.assertEqual(
            {row["item_ids"][0] for row in act["executors"]},
            {
                implementation_item["id"],
                declaration_item["id"],
                test_item["id"],
            },
        )
        self.assertEqual(
            {row["path"] for row in act["transitions"]},
            {"src/api.c", "include/api.h", "test/test_api.c"},
        )
        self.assertEqual(implementation.read_text(), original_implementation)
        self.assertEqual(declaration.read_text(), original_declaration)
        self.assertEqual(test_source.read_text(), original_test)
        self.assertEqual(plan_path.read_bytes(), plan_bytes)

        applied = run(
            "apply", str(act_path), "--root", str(self.root)
        ).stdout.decode()
        self.assertEqual(
            applied, "Result: applied-transitions=3; state=applied\n"
        )
        self.assertEqual(implementation.read_text(), completed_implementation)
        self.assertEqual(declaration.read_text(), completed_declaration)
        self.assertEqual(test_source.read_text(), completed_test)
        verification = json.loads(
            run(
                "verify",
                "--root",
                str(self.root),
                "--format",
                "json",
                "--check",
            ).stdout
        )
        self.assertEqual(verification["summary"]["constraints"]["pass"], 3)
        self.assertEqual(
            run(
                "apply", str(act_path), "--root", str(self.root)
            ).stdout.decode(),
            "Result: applied-transitions=0; state=already-satisfied\n",
        )

    def test_asserted_file_submission_grounds_plan_only_at_act_boundary(
        self,
    ) -> None:
        self.configure(
            {
                "IMPLEMENT-API": {
                    "kind": "required_symbols",
                    "symbols": ["future_api"],
                    "paths": ["module.py"],
                    "kinds": ["function"],
                    "owner": "architecture",
                    "rationale": "The reviewed API implementation exists.",
                }
            }
        )
        source = self.root / "module.py"
        original = "def current_api():\n    return 1\n"
        source.write_text(original)
        plan_path = self.plan_path("submitted-api-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan_bytes = plan_path.read_bytes()
        plan = json.loads(plan_bytes)
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertEqual(
            item["operation"],
            {
                "action": "add_symbol",
                "kinds": ["function"],
                "path": "module.py",
                "symbol": "future_api",
            },
        )
        self.assertFalse(item["executable"])

        blocked = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--format",
            "json",
            expected=2,
        )
        self.assertIn("manual or blocked item", blocked.stderr.decode())

        bad_replacement = self.plan_path("bad-module.py")
        bad_replacement.write_text("def current_api():\n    return 2\n")
        rejected = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{item['id']}={bad_replacement}",
            "--format",
            "json",
            expected=2,
        )
        self.assertIn("failing constraint", rejected.stderr.decode())
        self.assertEqual(source.read_text(), original)
        self.assertEqual(plan_path.read_bytes(), plan_bytes)

        replacement = self.plan_path("module.py")
        completed_source = (
            original + "\n\ndef future_api():\n    return 2\n"
        )
        replacement.write_text(completed_source)
        act_path = self.plan_path("submitted-api-act.json")
        run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{item['id']}={replacement}",
            "--format",
            "json",
            "--output",
            str(act_path),
        )
        act = json.loads(act_path.read_bytes())
        self.assertEqual(act["state"], "accepted")
        self.assertEqual(act["acceptance"]["status"], "satisfied")
        self.assertEqual(len(act["executors"]), 1)
        self.assertEqual(
            act["executors"][0]["capability"],
            "archbird.asserted.source.replace-file@1",
        )
        self.assertEqual(act["executors"][0]["item_ids"], [item["id"]])
        self.assertEqual(act["executors"][0]["matches"], 1)
        self.assertEqual(act["executors"][0]["reads"], ["module.py"])
        self.assertEqual(act["executors"][0]["writes"], ["module.py"])
        self.assertEqual(act["transitions"][0]["path"], "module.py")
        self.assertEqual(source.read_text(), original)
        self.assertEqual(plan_path.read_bytes(), plan_bytes)

        source.write_text("def drifted():\n    return 3\n")
        drift = run(
            "apply",
            str(act_path),
            "--root",
            str(self.root),
            expected=2,
        )
        self.assertIn("module.py", drift.stderr.decode())
        source.write_text(original)
        run("apply", str(act_path), "--root", str(self.root))
        self.assertEqual(source.read_text(), completed_source)
        run("verify", "--root", str(self.root), "--check")
        replay = run(
            "apply", str(act_path), "--root", str(self.root)
        ).stdout.decode()
        self.assertEqual(
            replay,
            "Result: applied-transitions=0; state=already-satisfied\n",
        )

        unknown = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{item['id']}={replacement}",
            "--submit",
            f"unknown-item={replacement}",
            "--format",
            "json",
            expected=2,
        )
        self.assertIn(
            "submission does not match an eligible Plan item",
            unknown.stderr.decode(),
        )

    def test_asserted_test_submission_uses_one_exact_mapped_test_file(
        self,
    ) -> None:
        self.configure(
            {
                "TEST-API": {
                    "kind": "required_test_route",
                    "group": "python",
                    "target": "module.py",
                    "selectors": ["test_future"],
                    "owner": "architecture",
                    "rationale": "The reviewed API has a focused test route.",
                }
            },
            layers=[
                {
                    "name": "python",
                    "language": "python",
                    "globs": ["**/*.py"],
                    "import_roots": ["."],
                }
            ],
            tests=[
                {
                    "name": "python",
                    "language": "python",
                    "globs": ["tests/test_*.py"],
                    "route_to": ["python"],
                }
            ],
        )
        (self.root / "module.py").write_text(
            "def future_api():\n    return 2\n"
        )
        tests = self.root / "tests"
        tests.mkdir()
        test_source = tests / "test_module.py"
        original = "def test_current():\n    assert True\n"
        test_source.write_text(original)

        plan_path = self.plan_path("test-route-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan_bytes = plan_path.read_bytes()
        plan = json.loads(plan_bytes)
        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertEqual(
            item["operation"],
            {
                "action": "add_test_route",
                "group": "python",
                "path": "tests/test_module.py",
                "selectors": ["test_future"],
                "target": "module.py",
            },
        )
        self.assertFalse(item["executable"])

        replacement = self.plan_path("test_module.py")
        replacement.write_text(
            "from module import future_api\n\n"
            "def test_future():\n"
            "    assert future_api() == 2\n"
        )
        act_path = self.plan_path("test-route-act.json")
        run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{item['id']}={replacement}",
            "--format",
            "json",
            "--output",
            str(act_path),
        )
        act = json.loads(act_path.read_bytes())
        self.assertEqual(act["acceptance"]["status"], "satisfied")
        self.assertEqual(
            act["executors"][0]["capability"],
            "archbird.asserted.source.replace-file@1",
        )
        self.assertEqual(act["transitions"][0]["path"], "tests/test_module.py")
        self.assertEqual(test_source.read_text(), original)
        self.assertEqual(plan_path.read_bytes(), plan_bytes)

        run("apply", str(act_path), "--root", str(self.root))
        self.assertIn("future_api()", test_source.read_text())
        verified = run(
            "verify",
            "--root",
            str(self.root),
            "--format",
            "json",
            "--check",
        )
        self.assertEqual(
            json.loads(verified.stdout)["summary"]["constraints"]["pass"],
            1,
        )

    def test_ambiguous_test_group_does_not_authorize_submission(self) -> None:
        self.configure(
            {
                "TEST-API": {
                    "kind": "required_test_route",
                    "group": "python",
                    "target": "module.py",
                    "selectors": ["test_future"],
                    "owner": "architecture",
                    "rationale": "The reviewed API has a focused test route.",
                }
            },
            layers=[
                {
                    "name": "python",
                    "language": "python",
                    "globs": ["**/*.py"],
                    "import_roots": ["."],
                }
            ],
            tests=[
                {
                    "name": "python",
                    "language": "python",
                    "globs": ["tests/test_*.py"],
                    "route_to": ["python"],
                }
            ],
        )
        (self.root / "module.py").write_text(
            "def future_api():\n    return 2\n"
        )
        tests = self.root / "tests"
        tests.mkdir()
        (tests / "test_first.py").write_text(
            "def test_first():\n    assert True\n"
        )
        (tests / "test_second.py").write_text(
            "def test_second():\n    assert True\n"
        )
        plan_path = self.plan_path("ambiguous-test-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        plan = json.loads(plan_path.read_bytes())
        item = plan["items"][0]
        self.assertNotIn("path", item["operation"])

        replacement = self.plan_path("ambiguous-test.py")
        replacement.write_text(
            "from module import future_api\n\n"
            "def test_future():\n"
            "    assert future_api() == 2\n"
        )
        rejected = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{item['id']}={replacement}",
            "--format",
            "json",
            expected=2,
        )
        self.assertIn("manual or blocked item", rejected.stderr.decode())

    def test_generated_test_file_does_not_authorize_direct_submission(
        self,
    ) -> None:
        self.configure(
            {
                "TEST-API": {
                    "kind": "required_test_route",
                    "group": "python",
                    "target": "module.py",
                    "selectors": ["test_future"],
                    "owner": "architecture",
                    "rationale": "The reviewed API has a focused test route.",
                }
            },
            layers=[
                {
                    "name": "python",
                    "language": "python",
                    "globs": ["**/*.py"],
                    "import_roots": ["."],
                }
            ],
            tests=[
                {
                    "name": "python",
                    "language": "python",
                    "globs": ["tests/test_*.py"],
                    "route_to": ["python"],
                    "generated_files": [
                        {
                            "globs": ["tests/test_generated.py"],
                            "sources": ["tests/test_source.py"],
                        }
                    ],
                }
            ],
        )
        (self.root / "module.py").write_text(
            "def future_api():\n    return 2\n"
        )
        tests = self.root / "tests"
        tests.mkdir()
        (tests / "test_source.py").write_text("CASES = ['current']\n")
        (tests / "test_generated.py").write_text(
            "def test_current():\n    assert True\n"
        )
        plan_path = self.plan_path("generated-test-plan.json")
        run("plan", "--root", str(self.root), "--output", str(plan_path))
        item = json.loads(plan_path.read_bytes())["items"][0]
        self.assertNotIn("path", item["operation"])

        replacement = self.plan_path("generated-test.py")
        replacement.write_text(
            "from module import future_api\n\n"
            "def test_future():\n"
            "    assert future_api() == 2\n"
        )
        rejected = run(
            "act",
            str(plan_path),
            "--root",
            str(self.root),
            "--submit",
            f"{item['id']}={replacement}",
            "--format",
            "json",
            expected=2,
        )
        self.assertIn("manual or blocked item", rejected.stderr.decode())

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
        self.assertEqual(
            applied,
            "Result: applied-transitions=0; state=already-satisfied\n",
        )

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
        created = self.root / "created.py"
        created.write_text("created = True\n")
        partial = run(
            "apply",
            str(act_path),
            "--root",
            str(self.root),
            expected=2,
        )
        self.assertIn("partially applied", partial.stderr.decode())
        created.unlink()
        run("apply", str(act_path), "--root", str(self.root))
        self.assertFalse(old.exists())
        self.assertEqual((self.root / "new/name.py").read_text(), "value = 1\n")
        self.assertEqual(created.read_text(), "created = True\n")
        replay = run(
            "apply",
            str(act_path),
            "--root",
            str(self.root),
        ).stdout.decode()
        self.assertEqual(
            replay,
            "Result: applied-transitions=0; state=already-satisfied\n",
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
