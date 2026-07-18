#!/usr/bin/env python3
"""Conformance probes for the native source/provider evidence validators."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "test" / "fixtures"


def canonical(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def invoke(executable: Path, mode: str, value: object) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(executable), mode],
        input=canonical(value),
        capture_output=True,
        check=False,
    )


def expect_rejected(executable: Path, mode: str, name: str, value: object) -> None:
    completed = invoke(executable, mode, value)
    if completed.returncode == 0:
        raise AssertionError(f"{name}: invalid evidence was accepted")
    if b"error 8" not in completed.stderr and b"error 6" not in completed.stderr:
        raise AssertionError(f"{name}: unexpected error: {completed.stderr!r}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_evidence.py JSON_CLI")
    executable = Path(sys.argv[1]).resolve()
    manifest = json.loads((FIXTURES / "source_manifest.valid.json").read_text())
    provider = json.loads((FIXTURES / "provider_facts.valid.json").read_text())

    for mode, value in (("--manifest", manifest), ("--provider", provider)):
        first = invoke(executable, mode, value)
        second = invoke(executable, mode, value)
        if first.returncode or second.returncode:
            raise AssertionError(first.stderr.decode() or second.stderr.decode())
        if (first.stdout, first.stderr) != (second.stdout, second.stderr):
            raise AssertionError(f"{mode}: validation result is not repeatable")

    resolved_manifest = deepcopy(manifest)
    resolved_manifest["configuration_sha256"] = "2" * 64
    resolved_manifest["resolution"] = {
        "coverage": {
            "assets": 4,
            "ignored": 1,
            "inventory_files": 9,
            "oversized": 1,
            "pruned_directories": 2,
            "selected": 2,
            "unsupported_known": 1,
        },
        "profile": {
            "implementation_sha256": "3" * 64,
            "name": "archbird-discovery-v1",
        },
        "sha256": "4" * 64,
    }
    if invoke(executable, "--manifest", resolved_manifest).returncode:
        raise AssertionError("valid discovery resolution evidence was rejected")

    manifest_digest = hashlib.sha256(canonical(manifest)).hexdigest()
    if provider["inputs"][0]["source_manifest_sha256"] != manifest_digest:
        raise AssertionError("provider fixture is not bound to canonical manifest bytes")

    invalid_manifest = []
    value = deepcopy(manifest)
    value["files"].reverse()
    invalid_manifest.append(("unsorted-files", value))
    value = deepcopy(manifest)
    value["files"][0]["path"] = "../escape.py"
    invalid_manifest.append(("parent-path", value))
    value = deepcopy(manifest)
    value["files"][0]["roles"] = ["source", "source"]
    invalid_manifest.append(("duplicate-role", value))
    value = deepcopy(manifest)
    value["files"][0]["bytes"] = 1.5
    invalid_manifest.append(("real-byte-count", value))
    value = deepcopy(manifest)
    value["surprise"] = True
    invalid_manifest.append(("unknown-field", value))
    value = deepcopy(resolved_manifest)
    value["resolution"]["coverage"]["selected"] = 1
    invalid_manifest.append(("resolution-selected-mismatch", value))
    value = deepcopy(resolved_manifest)
    value["resolution"]["coverage"]["selected"] = 1.0
    invalid_manifest.append(("resolution-real-count", value))

    invalid_provider = []
    value = deepcopy(provider)
    value["facts"][0]["claim"] = "semantic-reference"
    invalid_provider.append(("undeclared-claim", value))
    value = deepcopy(provider)
    value["facts"][0]["path"] = "py/other.py"
    invalid_provider.append(("outside-file-subject", value))
    value = deepcopy(provider)
    value["facts"][0]["span"] = {"start": 10, "end": 2}
    invalid_provider.append(("reversed-span", value))
    value = deepcopy(provider)
    value["facts"][0]["project"] = "other"
    invalid_provider.append(("unbound-project", value))
    value = deepcopy(provider)
    value["resolutions"] = [
        {"fact_id": "symbol:answer:0", "state": "unique", "targets": []}
    ]
    invalid_provider.append(("resolution-cardinality", value))
    value = deepcopy(provider)
    value["subject"]["name"] = "unexpected"
    invalid_provider.append(("subject-extra-field", value))
    value = deepcopy(provider)
    value["capabilities"][0]["coverage"] = "none"
    invalid_provider.append(("none-with-facts", value))
    value = deepcopy(provider)
    value["facts"][0]["correlation"] = "name"
    invalid_provider.append(("invalid-correlation", value))

    for name, value in invalid_manifest:
        expect_rejected(executable, "--manifest", name, value)
    for name, value in invalid_provider:
        expect_rejected(executable, "--provider", name, value)

    print(
        "native evidence validators passed: "
        f"3 valid, {len(invalid_manifest) + len(invalid_provider)} invalid"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
