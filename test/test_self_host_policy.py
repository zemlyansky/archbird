"""Exercise source-versus-context stability policy in the self-host gate."""

from __future__ import annotations

import copy
from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import self_host, sync_csrc


def fixture() -> dict[str, object]:
    return {
        "artifact": "map",
        "discovery": {
            "coverage": {
                "assets": 2,
                "ignored": 1,
                "inventory_files": 5,
                "oversized": 0,
                "pruned_directories": 1,
                "selected": 1,
                "unsupported_known": 0,
            },
            "profile": {
                "implementation_sha256": "1" * 64,
                "name": "archbird-discovery-v1",
            },
            "sha256": "2" * 64,
        },
        "evidence": {
            "absolute_paths_included": False,
            "config_sha256": "3" * 64,
            "input_sha256": "4" * 64,
        },
        "files": [
            {
                "bytes": 8,
                "language": "c",
                "path": "src/main.c",
                "sha256": "5" * 64,
                "symbols": [],
            }
        ],
        "project": "fixture",
        "schema_version": 7,
        "tool": {
            "implementation_sha256": "6" * 64,
            "name": "archbird",
            "version": "fixture",
        },
    }


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    if (
        self_host.core_source_digest(root)
        != sync_csrc.repository_implementation_digest()
    ):
        raise AssertionError(
            "self-host source closure differs from package implementation identity"
        )

    before = fixture()

    context_drift = copy.deepcopy(before)
    context_drift["discovery"]["coverage"]["pruned_directories"] = 3
    context_drift["discovery"]["sha256"] = "7" * 64
    context_result = self_host.test_stability(before, context_drift)
    if (
        not context_result["source_evidence"]["equal"]
        or context_result["discovery_context"]["equal"]
    ):
        raise AssertionError("discovery-only drift was not isolated from source evidence")

    producer_drift = copy.deepcopy(before)
    producer_drift["tool"]["implementation_sha256"] = "8" * 64
    producer_result = self_host.test_stability(before, producer_drift)
    if (
        not producer_result["source_evidence"]["equal"]
        or not producer_result["discovery_context"]["equal"]
    ):
        raise AssertionError("producer identity entered the source-stability projection")

    changed_source = copy.deepcopy(before)
    changed_source["files"][0]["sha256"] = "9" * 64
    changed_source["evidence"]["input_sha256"] = "a" * 64
    if self_host.test_stability(before, changed_source)["source_evidence"]["equal"]:
        raise AssertionError("mapped source drift was accepted")

    changed_structure = copy.deepcopy(before)
    changed_structure["edges"] = [
        {"kind": "call", "source": "src/main.c", "target": "src/helper.c"}
    ]
    if self_host.test_stability(before, changed_structure)["source_evidence"]["equal"]:
        raise AssertionError("derived structural drift was accepted")

    print("self-host source/context stability policy passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
