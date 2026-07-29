#!/usr/bin/env python3
"""Require Python and Node Act previews to emit identical file transitions."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from archbird.acting import preview_plan


def _sha(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _item(identifier: str, operation: dict[str, object]) -> dict[str, object]:
    return {
        "id": identifier,
        "statement": identifier,
        "provenance": "asserted",
        "origins": [
            {
                "constraint_id": "PORTABLE-PATCH",
                "constraint_result_sha256": "1" * 64,
                "issue_fingerprint": hashlib.sha256(
                    identifier.encode()
                ).hexdigest(),
            }
        ],
        "evidence": [],
        "depends_on": [],
        "operation": operation,
        "acceptance": {"constraints": ["PORTABLE-PATCH"]},
        "unknowns": [],
        "executable": True,
        "non_executable_reasons": [],
    }


def _plan(items: list[dict[str, object]]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "artifact": "plan",
        "provenance": "asserted",
        "tool": {
            "name": "archbird",
            "version": "test",
            "implementation_sha256": "2" * 64,
        },
        "source": {
            "project": "frontend-parity",
            "map": {
                "sha256": "3" * 64,
                "input_sha256": "4" * 64,
                "configuration_sha256": "5" * 64,
                "producer_implementation_sha256": "6" * 64,
            },
            "verification": {
                "sha256": "7" * 64,
                "policy_sha256": "8" * 64,
                "producer_implementation_sha256": "9" * 64,
            },
        },
        "objective": "Exercise portable Act rendering.",
        "items": items,
        "preserved_constraints": [],
        "unknowns": [],
    }


class PlanActFrontendParityTest(unittest.TestCase):
    def test_python_and_node_preview_are_byte_identical(self) -> None:
        node_argument = sys.argv[1] if len(sys.argv) > 1 else "node"
        node = (
            Path(node_argument).resolve()
            if "/" in node_argument
            else Path(shutil.which(node_argument) or node_argument)
        )
        addon = (
            Path(sys.argv[2]).resolve()
            if len(sys.argv) > 2
            else ROOT / "js/build/Release/_native.node"
        )
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            repository = Path(temporary)
            repeated = b"a\nb\na\n"
            unicode_source = "café\r\nlast".encode()
            moved = b"move without newline"
            binary = b"\xff\x00\x01"
            for relative, value in (
                ("repeat.txt", repeated),
                ("unicode.txt", unicode_source),
                ("old.txt", moved),
                ("binary.dat", binary),
            ):
                (repository / relative).write_bytes(value)
            name = "café".encode()
            start = unicode_source.index(name)
            document = _plan(
                [
                    _item(
                        "reorder",
                        {
                            "action": "replace_range",
                            "path": "repeat.txt",
                            "source_sha256": _sha(repeated),
                            "start_byte": 0,
                            "end_byte": len(repeated),
                            "before": repeated.decode(),
                            "replacement": "a\na\nb\n",
                        },
                    ),
                    _item(
                        "unicode",
                        {
                            "action": "replace_range",
                            "path": "unicode.txt",
                            "source_sha256": _sha(unicode_source),
                            "start_byte": start,
                            "end_byte": start + len(name),
                            "before": "café",
                            "replacement": "coffee",
                        },
                    ),
                    _item(
                        "move",
                        {
                            "action": "move_file",
                            "source_path": "old.txt",
                            "destination_path": "new.txt",
                            "source_sha256": _sha(moved),
                        },
                    ),
                    _item(
                        "delete-binary",
                        {
                            "action": "delete_file",
                            "path": "binary.dat",
                            "source_sha256": _sha(binary),
                        },
                    ),
                    _item(
                        "create",
                        {
                            "action": "create_file",
                            "path": "created.txt",
                            "content": "created without newline",
                        },
                    ),
                ]
            )
            plan_path = repository / "plan.json"
            plan_path.write_text(
                json.dumps(document, ensure_ascii=False, sort_keys=True),
                encoding="utf-8",
            )
            python_result = preview_plan(document, repository)
            environment = {
                **os.environ,
                "ARCHBIRD_ENGINE": "native",
                "ARCHBIRD_NATIVE_ADDON": str(addon),
            }
            completed = subprocess.run(
                [
                    str(node),
                    str(ROOT / "js/src/cli.js"),
                    "act",
                    str(plan_path),
                    "--root",
                    str(repository),
                    "--format",
                    "json",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
                check=False,
            )
            self.assertEqual(
                completed.returncode,
                0,
                completed.stderr.decode(errors="replace"),
            )
            node_result = json.loads(completed.stdout)
            self.assertEqual(python_result["status"], "preview")
            self.assertEqual(node_result["status"], "preview")
            self.assertEqual(
                python_result["plan_sha256"],
                node_result["plan_sha256"],
            )
            self.assertEqual(python_result["changes"], node_result["changes"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
