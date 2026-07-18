#!/usr/bin/env python3
"""Check deterministic native provider-merge ledger semantics."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_merge_ledger.py PROJECT_TEST")
    executable = Path(sys.argv[1]).resolve()
    first = subprocess.run(
        [str(executable), "--ledger"], capture_output=True, check=True
    )
    second = subprocess.run(
        [str(executable), "--ledger"], capture_output=True, check=True
    )
    if first.stdout != second.stdout:
        raise AssertionError("merge ledger bytes changed across identical runs")
    data = json.loads(first.stdout)
    if data["artifact"] != "archbird-provider-merge" or data["schema_version"] != 5:
        raise AssertionError("unexpected merge artifact identity")
    if data["summary"] != {
        "audit_differences": 1,
        "audit_matches": 0,
        "conflicts": 1,
        "contributed": 1,
        "deduplicated": 1,
        "selections": 1,
        "enriched": 1,
        "providers": 5,
        "selected_facts": 1,
        "variations": 1,
    }:
        raise AssertionError(f"unexpected summary: {data['summary']!r}")
    provider_hashes = [row["sha256"] for row in data["providers"]]
    if provider_hashes != sorted(provider_hashes) or len(set(provider_hashes)) != 5:
        raise AssertionError("providers are not deterministically digest-ordered")
    if data["selections"][0]["domain"] != "symbols":
        raise AssertionError("symbols domain missing")
    if data["selections"][0]["subject"] != {
        "scope": "file",
        "project": "sample",
        "path": "src/a.txt",
    }:
        raise AssertionError("selection subject missing")
    if len(data["occurrences"]) != 1:
        raise AssertionError("merged occurrence ledger is incomplete")
    occurrence = data["occurrences"][0]
    if occurrence["domain"] != "symbols":
        raise AssertionError("merged occurrence domain is absent")
    if occurrence["canonical"]["claim"] != "syntax-structure":
        raise AssertionError("primary claim did not remain canonical")
    claims = [row["claim"] for row in occurrence["contributors"]]
    if len(claims) != 3 or "lexical-occurrence" not in claims:
        raise AssertionError(f"supporting claims were lost: {claims!r}")
    conflict = data["conflicts"][0]
    if conflict["reason"] != "augment-mismatch" or conflict["witness"] != {
        "end": 3,
        "key": "a",
        "kind": "variable",
        "path": "src/a.txt",
        "start": 0,
    }:
        raise AssertionError(f"unexpected conflict witness: {conflict!r}")
    if conflict["left_fact"]["name"] != "a" or conflict["right_fact"]["name"] != "b":
        raise AssertionError(f"conflicting fact projections are incomplete: {conflict!r}")
    if data["variations"] != [
        {
            "alternate_provider": next(
                row["sha256"]
                for row in data["providers"]
                if row["producer"]["name"] == "fixture-enriched"
            ),
            "alternate_value": "a = 1",
            "attribute": "signature",
            "canonical_provider": occurrence["canonical"]["provider"],
            "canonical_value": "a=1",
            "domain": "symbols",
            "subject": {
                "scope": "file",
                "project": "sample",
                "path": "src/a.txt",
            },
            "witness": {
                "end": 3,
                "key": "a",
                "kind": "variable",
                "path": "src/a.txt",
                "start": 0,
            },
        }
    ]:
        raise AssertionError(f"unexpected presentation variations: {data['variations']!r}")
    print(
        "native merge ledger passed: "
        f"{len(data['providers'])} providers, {len(data['conflicts'])} conflict"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
