#!/usr/bin/env python3
"""Pin portable and bounded Plan ingestion in the Python host."""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from archbird import acting
from archbird._plan_limits import (
    MAX_COLLECTION_ITEMS,
    MAX_FILE_BYTES,
    MAX_METADATA_BYTES,
    MAX_OPERATION_TEXT_BYTES,
    MAX_SAFE_INTEGER,
)
from archbird.acting import preview_plan


def _sha(character: str) -> str:
    return character * 64


def _item(operation: dict[str, object]) -> dict[str, object]:
    return {
        "id": "edit",
        "statement": "edit",
        "provenance": "derived",
        "origins": [
            {
                "constraint_id": "TEST",
                "constraint_result_sha256": _sha("1"),
                "issue_fingerprint": _sha("2"),
            }
        ],
        "evidence": [
            {
                "provenance": "derived",
                "project": "fixture",
                "path": "source.txt",
                "line": 1,
                "sha256": _sha("3"),
                "detail": "source",
            }
        ],
        "depends_on": [],
        "operation": operation,
        "acceptance": {"constraints": ["TEST"]},
        "unknowns": [],
        "executable": True,
        "non_executable_reasons": [],
    }


def _plan(operation: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "artifact": "plan",
        "provenance": "derived",
        "tool": {
            "name": "archbird",
            "version": "test",
            "implementation_sha256": _sha("a"),
        },
        "source": {
            "project": "fixture",
            "map": {
                "sha256": _sha("b"),
                "input_sha256": _sha("c"),
                "configuration_sha256": _sha("d"),
                "producer_implementation_sha256": _sha("e"),
            },
            "verification": {
                "sha256": _sha("f"),
                "policy_sha256": _sha("4"),
                "producer_implementation_sha256": _sha("5"),
            },
        },
        "objective": "exercise resource bounds",
        "items": [_item(operation)],
        "preserved_constraints": [],
        "unknowns": [],
    }


class PlanResourceLimitsTest(unittest.TestCase):
    def test_coordinates_stop_at_javascript_safe_integer(self) -> None:
        document = _plan(
            {
                "action": "replace_range",
                "path": "source.txt",
                "source_sha256": _sha("3"),
                "start_byte": MAX_SAFE_INTEGER,
                "end_byte": MAX_SAFE_INTEGER,
                "before": "",
                "replacement": "",
            }
        )
        document["items"][0]["evidence"][0]["line"] = MAX_SAFE_INTEGER
        acting._validate_plan_shape(document)

        for field in ("start_byte", "end_byte"):
            invalid = _plan(dict(document["items"][0]["operation"]))
            invalid["items"][0]["operation"][field] = MAX_SAFE_INTEGER + 1
            with self.subTest(field=field):
                with self.assertRaises(ValueError):
                    acting._validate_plan_shape(invalid)

        document["items"][0]["evidence"][0]["line"] = MAX_SAFE_INTEGER + 1
        with self.assertRaises(ValueError):
            acting._validate_plan_shape(document)

    def test_metadata_operation_text_and_arrays_are_bounded(self) -> None:
        base = {
            "action": "create_file",
            "path": "created.txt",
            "content": "",
        }
        document = _plan(base)
        document["items"][0]["statement"] = "s" * (MAX_METADATA_BYTES + 1)
        with self.assertRaises(ValueError):
            acting._validate_plan_shape(document)

        document = _plan(base)
        document["items"][0]["operation"]["content"] = (
            "c" * (MAX_OPERATION_TEXT_BYTES + 1)
        )
        with self.assertRaises(ValueError):
            acting._validate_plan_shape(document)

        document = _plan(
            {
                "action": "manual",
                "instructions": "review",
                "candidate_paths": [
                    f"path-{index}" for index in range(MAX_COLLECTION_ITEMS + 1)
                ],
            }
        )
        document["items"][0]["executable"] = False
        document["items"][0]["non_executable_reasons"] = ["review"]
        with self.assertRaises(ValueError):
            acting._validate_plan_shape(document)

    def test_oversized_sparse_source_is_rejected_before_reading(self) -> None:
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            repository = Path(temporary)
            target = repository / "source.txt"
            with target.open("wb") as stream:
                stream.truncate(MAX_FILE_BYTES + 1)
            document = _plan(
                {
                    "action": "delete_file",
                    "path": "source.txt",
                    "source_sha256": hashlib.sha256(b"").hexdigest(),
                }
            )
            result = preview_plan(document, repository)
            self.assertEqual(result["status"], "blocked")
            self.assertEqual(result["changes"], [])
            self.assertTrue(
                any("67108864" in row["message"] for row in result["diagnostics"]),
                result,
            )

    def test_move_counts_source_and_destination_as_touched_files(self) -> None:
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            repository = Path(temporary)
            source = b"source\n"
            (repository / "source.txt").write_bytes(source)
            document = _plan(
                {
                    "action": "move_file",
                    "source_path": "source.txt",
                    "destination_path": "destination.txt",
                    "source_sha256": hashlib.sha256(source).hexdigest(),
                }
            )
            with mock.patch.object(acting, "MAX_TOUCHED_FILES", 1):
                result = preview_plan(document, repository)
            self.assertEqual(result["status"], "blocked")
            self.assertTrue(
                any("touches more than 1 files" in row["message"]
                    for row in result["diagnostics"]),
                result,
            )


if __name__ == "__main__":
    unittest.main()
