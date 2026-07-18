#!/usr/bin/env python3
"""Source-content locks must control derived verification evidence."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def suite(expected: str | None) -> dict:
    project = {"map": "subject.map.json", "revision": "rev-a"}
    if expected is not None:
        project["source_lock"] = {"src/value.c": expected}
    return {
        "schema_version": 1,
        "suite": "source-lock",
        "projects": {"subject": project},
        "extractors": {
            "expected.names": {"kind": "literal_set", "values": ["A"]},
            "expected.values": {"kind": "literal_values", "values": {"A": 1}},
            "subject.names": {"kind": "literal_set", "values": ["A", "B"]},
            "subject.symbols": {"kind": "symbols", "project": "subject"},
            "subject.values": {
                "kind": "literal_values",
                "values": {"A": 1, "B": 9},
            },
        },
        "checks": [
            {
                "id": "SOURCE-LOCK-SYMBOLS",
                "assert": "cardinality",
                "actual": "subject.symbols",
                "min": 1,
                "owner": "test",
                "rationale": "Only current locked bytes may support derived facts.",
            },
            {
                "id": "REQUIRED-SUBSET",
                "assert": "required_subset",
                "expected": "expected.names",
                "actual": "subject.names",
                "owner": "test",
                "rationale": "Required names must exist while subject-only names remain valid.",
            },
            {
                "id": "REQUIRED-VALUES",
                "assert": "required_values",
                "expected": "expected.values",
                "actual": "subject.values",
                "owner": "test",
                "rationale": "Required values must match while subject-only values remain valid.",
            },
        ],
    }


def evidence(source: bytes, *, include_source: bool = True) -> dict:
    digest = hashlib.sha256(source).hexdigest()
    return {
        "schema_version": 1,
        "artifact": "verification-input",
        "suite_path": "source-lock.verify.json",
        "projects": [
            {
                "name": "subject",
                "map": {
                    "artifact": "map",
                    "schema_version": 6,
                    "project": "source-lock-fixture",
                    "tool": {
                        "name": "archbird",
                        "version": "fixture",
                        "implementation_sha256": "1" * 64,
                    },
                    "evidence": {
                        "config_sha256": "2" * 64,
                        "input_sha256": "3" * 64,
                    },
                    "diagnostics": [],
                    "files": [
                        {
                            "path": "src/value.c",
                            "layer": "core",
                            "sha256": digest,
                            "symbols": [
                                {
                                    "name": "value",
                                    "kind": "function",
                                    "scope": "public",
                                    "line": 1,
                                }
                            ],
                        }
                    ],
                },
                "sources": (
                    [{"path": "src/value.c", "text": source.decode()}]
                    if include_source
                    else []
                ),
            }
        ],
        "provided_facts": [],
        "attestations": [],
        "baseline": None,
    }


def analyze(extension, document: dict, input_document: dict) -> dict:
    return json.loads(
        extension.verification_analyze(
            canonical(document), canonical(input_document)
        )
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_verify_source_lock.py EXTENSION")
    extension = load_extension(Path(sys.argv[1]).resolve())
    source = b"int value(void) { return 1; }\n"
    digest = hashlib.sha256(source).hexdigest()

    plan = json.loads(extension.verification_plan(canonical(suite(digest))))
    lock_sources = [row for row in plan["sources"] if row["kind"] == "source_lock"]
    if lock_sources != [
        {
            "extractor": "",
            "kind": "source_lock",
            "path": "src/value.c",
            "project": "subject",
            "provider": "content-lock",
        }
    ]:
        raise AssertionError(lock_sources)

    current = analyze(extension, suite(digest), evidence(source))
    project = current["projects"][0]
    if project["revision_provenance"] != "asserted":
        raise AssertionError(project)
    if project["source_lock"]["state"] != "current":
        raise AssertionError(project)
    current_checks = {row["id"]: row for row in current["checks"]}
    if {name: row["status"] for name, row in current_checks.items()} != {
        "REQUIRED-SUBSET": "pass",
        "REQUIRED-VALUES": "pass",
        "SOURCE-LOCK-SYMBOLS": "pass",
    }:
        raise AssertionError(current_checks)

    missing_suite = suite(digest)
    missing_suite["extractors"]["expected.names"]["values"].append("C")
    missing = analyze(extension, missing_suite, evidence(source))
    missing_check = next(
        row for row in missing["checks"] if row["id"] == "REQUIRED-SUBSET"
    )
    if missing_check["status"] != "fail" or [
        finding["comparison"] for finding in missing_check["findings"]
    ] != ["missing"]:
        raise AssertionError(missing_check)
    fingerprint = missing_check["findings"][0]["fingerprint"]
    proposal = json.loads(
        extension.change_proposal(canonical(missing), fingerprint)
    )
    if proposal["origin"]["finding"]["fingerprint"] != fingerprint:
        raise AssertionError(proposal["origin"])
    stale_finding = json.loads(json.dumps(missing))
    next(
        row
        for row in stale_finding["checks"]
        if row["id"] == "REQUIRED-SUBSET"
    )["findings"][0]["evidence_state"] = "stale"
    try:
        extension.change_proposal(canonical(stale_finding), fingerprint)
    except extension.Error as error:
        if "evidence must be current" not in str(error):
            raise
    else:
        raise AssertionError("Act must reject stale finding evidence")

    mismatch = analyze(extension, suite("0" * 64), evidence(source))
    if mismatch["projects"][0]["source_lock"]["state"] != "mismatch":
        raise AssertionError(mismatch["projects"][0])
    mismatch_fact = next(
        row for row in mismatch["facts"] if row["name"] == "subject.symbols"
    )
    if mismatch_fact["state"] != "stale":
        raise AssertionError(mismatch_fact)
    mismatch_check = next(
        row for row in mismatch["checks"] if row["id"] == "SOURCE-LOCK-SYMBOLS"
    )
    if mismatch_check["status"] != "unknown":
        raise AssertionError(mismatch_check)
    if not mismatch["summary"]["blocking"]:
        raise AssertionError(mismatch["summary"])
    mismatch_markdown = extension.verification_report(
        canonical(suite("0" * 64)),
        canonical(evidence(source)),
        "markdown",
    ).decode()
    lock_line = (
        "lock path=src/value.c state=mismatch expected="
        + "0" * 64
        + " actual="
        + digest
    )
    if lock_line not in mismatch_markdown:
        raise AssertionError(mismatch_markdown)

    unavailable = analyze(
        extension, suite(digest), evidence(source, include_source=False)
    )
    if unavailable["projects"][0]["source_lock"]["state"] != "unavailable":
        raise AssertionError(unavailable["projects"][0])
    unavailable_fact = next(
        row for row in unavailable["facts"] if row["name"] == "subject.symbols"
    )
    if unavailable_fact["state"] != "unknown":
        raise AssertionError(unavailable_fact)

    unlocked = analyze(extension, suite(None), evidence(source))
    if unlocked["projects"][0]["source_lock"]["state"] != "not_declared":
        raise AssertionError(unlocked["projects"][0])
    if any(row["status"] != "pass" for row in unlocked["checks"]):
        raise AssertionError(unlocked["checks"])

    invalid = suite(digest)
    invalid["projects"]["subject"]["source_lock"] = {"src/*.c": digest}
    try:
        extension.verification_plan(canonical(invalid))
    except extension.Error:
        pass
    else:
        raise AssertionError("source-lock patterns must be rejected")

    print("source-content lock provenance and staleness passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
