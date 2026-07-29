#!/usr/bin/env python3
"""Exercise virtual and applied Plan source transformations."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from archbird import _native
from archbird import acting
from archbird.acting import apply_plan, preview_plan


def sha(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def item(identifier: str, operation: dict, *, executable: bool = True) -> dict:
    return {
        "id": identifier,
        "statement": identifier,
        "provenance": "derived",
        "origins": [
            {
                "constraint_id": "TEST-CONSTRAINT",
                "constraint_result_sha256": "1" * 64,
                "issue_fingerprint": "2" * 64,
            }
        ],
        "evidence": [],
        "depends_on": [],
        "executable": executable,
        "non_executable_reasons": [] if executable else ["input_required"],
        "operation": operation,
        "acceptance": {"constraints": ["TEST-CONSTRAINT"]},
        "unknowns": [],
    }


def plan(*items: dict) -> dict:
    return {
        "schema_version": 1,
        "artifact": "plan",
        "provenance": "derived",
        "tool": {
            "name": "archbird",
            "version": "test",
            "implementation_sha256": "3" * 64,
        },
        "source": {
            "project": "test",
            "map": {
                "sha256": "4" * 64,
                "input_sha256": "5" * 64,
                "configuration_sha256": "6" * 64,
                "producer_implementation_sha256": "7" * 64,
            },
            "verification": {
                "sha256": "8" * 64,
                "policy_sha256": "9" * 64,
                "producer_implementation_sha256": "a" * 64,
            },
        },
        "objective": "exercise Act",
        "items": list(items),
        "preserved_constraints": [],
        "unknowns": [],
    }


def satisfied(_plan: object, _root: Path) -> dict:
    return {
        "status": "satisfied",
        "verification_sha256": "b" * 64,
        "constraints": [{"id": "TEST-CONSTRAINT", "status": "pass"}],
    }


class ActExecutionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write(self, path: str, value: bytes) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(value)

    def test_unicode_byte_offsets_and_multiple_edits(self) -> None:
        source = "const café = 'old';\nreturn café;\n".encode()
        self.write("src/main.js", source)
        old = source.index(b"old")
        name = source.index("café".encode())
        document = plan(
            item(
                "rename",
                {
                    "action": "replace_range",
                    "path": "src/main.js",
                    "source_sha256": sha(source),
                    "start_byte": name,
                    "end_byte": name + len("café".encode()),
                    "before": "café",
                    "replacement": "coffee",
                },
            ),
            item(
                "value",
                {
                    "action": "replace_range",
                    "path": "src/main.js",
                    "source_sha256": sha(source),
                    "start_byte": old,
                    "end_byte": old + 3,
                    "before": "old",
                    "replacement": "new",
                },
            ),
        )
        result = preview_plan(document, self.root)
        self.assertEqual(result["status"], "preview")
        self.assertEqual(result["changes"][0]["kind"], "modify")
        self.assertIn(
            "+const coffee = 'new';", result["changes"][0]["unified_diff"]
        )
        self.assertEqual((self.root / "src/main.js").read_bytes(), source)

    def test_create_delete_move_replace_preview_and_apply(self) -> None:
        edited = b"alpha = 1\n"
        deleted = b"obsolete\n"
        moved = b"move me\n"
        self.write("edited.py", edited)
        self.write("deleted.txt", deleted)
        self.write("old/name.txt", moved)
        document = plan(
            item(
                "edit",
                {
                    "action": "replace_range",
                    "path": "edited.py",
                    "source_sha256": sha(edited),
                    "start_byte": 8,
                    "end_byte": 9,
                    "before": "1",
                    "replacement": "2",
                },
            ),
            item(
                "create",
                {"action": "create_file", "path": "new/file.txt", "content": "new\n"},
            ),
            item(
                "delete",
                {
                    "action": "delete_file",
                    "path": "deleted.txt",
                    "source_sha256": sha(deleted),
                },
            ),
            item(
                "move",
                {
                    "action": "move_file",
                    "source_path": "old/name.txt",
                    "destination_path": "new/name.txt",
                    "source_sha256": sha(moved),
                },
            ),
        )
        before = {
            path.relative_to(self.root).as_posix(): path.read_bytes()
            for path in self.root.rglob("*")
            if path.is_file()
        }
        preview = preview_plan(document, self.root)
        self.assertEqual(preview["status"], "preview")
        self.assertEqual(
            {change["kind"] for change in preview["changes"]},
            {"modify", "create", "delete", "move"},
        )
        self.assertEqual(
            before,
            {
                path.relative_to(self.root).as_posix(): path.read_bytes()
                for path in self.root.rglob("*")
                if path.is_file()
            },
        )
        applied = apply_plan(document, self.root, satisfied)
        self.assertEqual(applied["status"], "applied")
        self.assertEqual(applied["plan_sha256"], preview["plan_sha256"])
        self.assertEqual((self.root / "edited.py").read_text(), "alpha = 2\n")
        self.assertEqual((self.root / "new/file.txt").read_text(), "new\n")
        self.assertEqual((self.root / "new/name.txt").read_bytes(), moved)
        self.assertFalse((self.root / "deleted.txt").exists())
        self.assertFalse((self.root / "old/name.txt").exists())

    def test_overlapping_edits_are_blocked(self) -> None:
        source = b"abcdef\n"
        self.write("source.txt", source)
        base = {
            "action": "replace_range",
            "path": "source.txt",
            "source_sha256": sha(source),
        }
        result = preview_plan(
            plan(
                item(
                    "left",
                    {
                        **base,
                        "start_byte": 1,
                        "end_byte": 4,
                        "before": "bcd",
                        "replacement": "B",
                    },
                ),
                item(
                    "right",
                    {
                        **base,
                        "start_byte": 3,
                        "end_byte": 5,
                        "before": "de",
                        "replacement": "D",
                    },
                ),
            ),
            self.root,
        )
        self.assertEqual(result["status"], "blocked")
        self.assertIn(
            "overlapping_edits",
            {row["code"] for row in result["diagnostics"]},
        )
        self.assertEqual((self.root / "source.txt").read_bytes(), source)

    def test_stale_source_and_before_text_are_blocked(self) -> None:
        source = b"value\n"
        self.write("source.txt", source)
        stale = preview_plan(
            plan(
                item(
                    "stale",
                    {
                        "action": "delete_file",
                        "path": "source.txt",
                        "source_sha256": "0" * 64,
                    },
                )
            ),
            self.root,
        )
        self.assertEqual(stale["status"], "blocked")
        before = preview_plan(
            plan(
                item(
                    "before",
                    {
                        "action": "replace_range",
                        "path": "source.txt",
                        "source_sha256": sha(source),
                        "start_byte": 0,
                        "end_byte": 5,
                        "before": "other",
                        "replacement": "new",
                    },
                )
            ),
            self.root,
        )
        self.assertEqual(before["status"], "blocked")

    def test_path_escape_absolute_symlink_and_non_regular_are_blocked(self) -> None:
        source = b"value\n"
        self.write("source.txt", source)
        (self.root / "link.txt").symlink_to("source.txt")
        (self.root / "directory").mkdir()
        cases = (
            (
                "escape",
                {
                    "action": "delete_file",
                    "path": "../outside",
                    "source_sha256": sha(source),
                },
            ),
            (
                "absolute",
                {
                    "action": "delete_file",
                    "path": str(self.root / "source.txt"),
                    "source_sha256": sha(source),
                },
            ),
            (
                "symlink",
                {
                    "action": "delete_file",
                    "path": "link.txt",
                    "source_sha256": sha(source),
                },
            ),
            (
                "directory",
                {
                    "action": "delete_file",
                    "path": "directory",
                    "source_sha256": sha(b""),
                },
            ),
        )
        for identifier, operation in cases:
            with self.subTest(identifier):
                result = preview_plan(plan(item(identifier, operation)), self.root)
                self.assertEqual(result["status"], "blocked")

        if hasattr(os, "mkfifo"):
            os.mkfifo(self.root / "pipe")
            fifo = preview_plan(
                plan(
                    item(
                        "fifo",
                        {
                            "action": "delete_file",
                            "path": "pipe",
                            "source_sha256": sha(b""),
                        },
                    )
                ),
                self.root,
            )
            self.assertEqual(fifo["status"], "blocked")

    def test_destination_conflicts_and_manual_items_are_blocked(self) -> None:
        self.write("exists.txt", b"existing\n")
        conflict = preview_plan(
            plan(
                item(
                    "create",
                    {
                        "action": "create_file",
                        "path": "exists.txt",
                        "content": "replacement\n",
                    },
                )
            ),
            self.root,
        )
        self.assertEqual(conflict["status"], "blocked")
        manual = preview_plan(
            plan(
                item(
                    "manual",
                    {
                        "action": "manual",
                        "instructions": "provide implementation",
                        "candidate_paths": ["source.c"],
                    },
                    executable=False,
                )
            ),
            self.root,
        )
        self.assertEqual(manual["status"], "blocked")
        self.assertEqual(manual["diagnostics"][0]["code"], "non_executable")

    def test_result_plan_digest_is_canonical(self) -> None:
        document = plan(
            item(
                "create",
                {"action": "create_file", "path": "created.txt", "content": "ok\n"},
            )
        )
        encoded = json.dumps(
            document,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        expected = hashlib.sha256(_native.json_canonicalize(encoded)).hexdigest()
        result = preview_plan(document, self.root)
        self.assertEqual(result["plan_sha256"], expected)
        self.assertEqual(result["status"], "preview")
        self.assertEqual(result["artifact"], "act-result")
        self.assertEqual(result["schema_version"], 1)

    def test_acceptance_failure_rolls_back_application(self) -> None:
        document = plan(
            item(
                "create",
                {"action": "create_file", "path": "created.txt", "content": "ok"},
            )
        )

        def fail(_plan: object, _root: Path) -> dict:
            raise RuntimeError("verification unavailable")

        result = apply_plan(document, self.root, fail)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["acceptance"]["status"], "not_evaluated")
        self.assertIn(
            "rolled back",
            result["diagnostics"][0]["message"].lower(),
        )
        self.assertFalse((self.root / "created.txt").exists())

    def test_noncanonical_plan_aliases_are_rejected(self) -> None:
        document = plan(
            item(
                "create",
                {"action": "create_file", "path": "created.txt", "content": "ok"},
            )
        )
        document["operations"] = document.pop("items")
        result = preview_plan(document, self.root)
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(result["changes"], [])
        self.assertEqual(result["diagnostics"][0]["code"], "invalid_plan")

    def test_duplicate_origins_and_evidence_are_rejected(self) -> None:
        operation = {"action": "create_file", "path": "new.txt", "content": "new\n"}
        duplicate_origin = item("duplicate-origin", operation)
        duplicate_origin["origins"].append(dict(duplicate_origin["origins"][0]))
        result = preview_plan(plan(duplicate_origin), self.root)
        self.assertEqual(result["status"], "blocked")
        self.assertIn("origins must contain unique", result["diagnostics"][0]["message"])

        evidence = {
            "provenance": "derived",
            "project": "test",
            "path": "source.py",
            "line": 1,
            "sha256": "c" * 64,
            "detail": "exact source witness",
        }
        duplicate_evidence = item("duplicate-evidence", operation)
        duplicate_evidence["evidence"] = [evidence, dict(evidence)]
        result = preview_plan(plan(duplicate_evidence), self.root)
        self.assertEqual(result["status"], "blocked")
        self.assertIn("evidence must contain unique", result["diagnostics"][0]["message"])

    def test_binary_diff_has_canonical_file_headers(self) -> None:
        source = b"\xff\x00\x01"
        self.write("binary.dat", source)
        result = preview_plan(
            plan(
                item(
                    "delete-binary",
                    {
                        "action": "delete_file",
                        "path": "binary.dat",
                        "source_sha256": sha(source),
                    },
                )
            ),
            self.root,
        )
        self.assertEqual(result["status"], "preview")
        rendered = result["changes"][0]["unified_diff"]
        self.assertIn("--- a/binary.dat\n+++ /dev/null\n", rendered)
        self.assertIn("Binary files a/binary.dat and /dev/null differ\n", rendered)

    def test_commit_failure_restores_already_replaced_files(self) -> None:
        first = b"first = 1\n"
        second = b"second = 1\n"
        self.write("first.py", first)
        self.write("second.py", second)
        document = plan(
            item(
                "first",
                {
                    "action": "replace_range",
                    "path": "first.py",
                    "source_sha256": sha(first),
                    "start_byte": 8,
                    "end_byte": 9,
                    "before": "1",
                    "replacement": "2",
                },
            ),
            item(
                "second",
                {
                    "action": "replace_range",
                    "path": "second.py",
                    "source_sha256": sha(second),
                    "start_byte": 9,
                    "end_byte": 10,
                    "before": "1",
                    "replacement": "2",
                },
            ),
        )
        replace = os.replace
        commits = 0

        def fail_second_commit(source: object, destination: object) -> None:
            nonlocal commits
            if Path(source).name.startswith("new-"):
                commits += 1
                if commits == 2:
                    raise OSError("injected commit failure")
            replace(source, destination)

        with mock.patch.object(acting.os, "replace", fail_second_commit):
            result = apply_plan(document, self.root, satisfied)
        self.assertEqual(result["status"], "failed")
        self.assertEqual((self.root / "first.py").read_bytes(), first)
        self.assertEqual((self.root / "second.py").read_bytes(), second)

    def test_acceptance_requires_exact_plan_constraint_coverage(self) -> None:
        callbacks = {
            "omitted": [
                {"id": "TEST-CONSTRAINT", "status": "pass"},
            ],
            "extra": [
                {"id": "TEST-CONSTRAINT", "status": "pass"},
                {"id": "PRESERVED", "status": "pass"},
                {"id": "EXTRA", "status": "pass"},
            ],
            "duplicate": [
                {"id": "TEST-CONSTRAINT", "status": "pass"},
                {"id": "TEST-CONSTRAINT", "status": "pass"},
                {"id": "PRESERVED", "status": "pass"},
            ],
        }
        for name, constraints in callbacks.items():
            with self.subTest(name):
                repository = self.root / name
                repository.mkdir()
                document = plan(
                    item(
                        "create",
                        {
                            "action": "create_file",
                            "path": "created.txt",
                            "content": "ok",
                        },
                    )
                )
                document["preserved_constraints"] = ["PRESERVED"]

                def acceptance(
                    _plan: object,
                    _root: Path,
                    rows: list[dict[str, str]] = constraints,
                ) -> dict:
                    return {
                        "status": "satisfied",
                        "verification_sha256": "b" * 64,
                        "constraints": rows,
                    }

                result = apply_plan(document, repository, acceptance)
                self.assertEqual(result["status"], "failed")
                self.assertEqual(
                    result["acceptance"]["status"],
                    "not_evaluated",
                )
                self.assertEqual(result["diagnostics"][0]["code"], "acceptance_failed")
                self.assertFalse((repository / "created.txt").exists())

        exact_root = self.root / "exact"
        exact_root.mkdir()
        exact = plan(
            item(
                "create",
                {
                    "action": "create_file",
                    "path": "created.txt",
                    "content": "ok",
                },
            )
        )
        exact["preserved_constraints"] = ["PRESERVED"]

        def exact_acceptance(_plan: object, _root: Path) -> dict:
            return {
                "status": "satisfied",
                "verification_sha256": "b" * 64,
                "constraints": [
                    {"id": "PRESERVED", "status": "waived"},
                    {"id": "TEST-CONSTRAINT", "status": "pass"},
                ],
            }

        exact_result = apply_plan(exact, exact_root, exact_acceptance)
        self.assertEqual(exact_result["status"], "applied")

        no_op_root = self.root / "no-op"
        no_op_root.mkdir()

        def empty_acceptance(_plan: object, _root: Path) -> dict:
            return {
                "status": "satisfied",
                "verification_sha256": "b" * 64,
                "constraints": [],
            }

        no_op_result = apply_plan(plan(), no_op_root, empty_acceptance)
        self.assertEqual(no_op_result["status"], "applied")
        self.assertEqual(no_op_result["changes"], [])

    def test_rejected_acceptance_is_evaluated_and_rolled_back(self) -> None:
        document = plan(
            item(
                "create",
                {
                    "action": "create_file",
                    "path": "created.txt",
                    "content": "ok",
                },
            )
        )

        for acceptance_status, constraint_status in (
            ("not_satisfied", "fail"),
            ("unknown", "unknown"),
        ):
            with self.subTest(acceptance_status):
                repository = self.root / acceptance_status
                repository.mkdir()

                def reject(_plan: object, _root: Path) -> dict:
                    return {
                        "status": acceptance_status,
                        "verification_sha256": "b" * 64,
                        "constraints": [
                            {
                                "id": "TEST-CONSTRAINT",
                                "status": constraint_status,
                            }
                        ],
                    }

                result = apply_plan(document, repository, reject)
                self.assertEqual(result["status"], "rejected")
                self.assertEqual(
                    result["acceptance"]["status"],
                    acceptance_status,
                )
                self.assertFalse((repository / "created.txt").exists())

    def test_acceptance_cannot_mutate_the_consumed_plan(self) -> None:
        document = plan(
            item(
                "create",
                {
                    "action": "create_file",
                    "path": "created.txt",
                    "content": "ok",
                },
            )
        )

        def mutate(callback_plan: object, _root: Path) -> dict:
            assert isinstance(callback_plan, dict)
            callback_plan["items"][0]["acceptance"]["constraints"] = ["OTHER"]
            return {
                "status": "satisfied",
                "verification_sha256": "b" * 64,
                "constraints": [{"id": "OTHER", "status": "pass"}],
            }

        result = apply_plan(document, self.root, mutate)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["acceptance"]["status"], "not_evaluated")
        self.assertFalse((self.root / "created.txt").exists())
        self.assertEqual(
            document["items"][0]["acceptance"]["constraints"],
            ["TEST-CONSTRAINT"],
        )


if __name__ == "__main__":
    unittest.main()
