from __future__ import annotations

import json
import hashlib
import difflib
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

from archbird.act.engine import (
    compile_change_proposal,
    create_change_contract,
    load_verification_artifact,
    verify_change_contract,
)
from archbird.act.render import (
    render_change_contract_json,
    render_change_proposal_json,
    render_change_result_json,
)
from archbird.verify.config import load_verification_suite
from archbird.verify.engine import verify
from archbird.verify.render import render_verification_json


def native(binary: Path, envelope: dict, command: str | None = None) -> bytes:
    process = subprocess.run(
        [str(binary), *([command] if command else [])],
        input=json.dumps(envelope, sort_keys=True, separators=(",", ":")).encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode:
        raise AssertionError(process.stderr.decode(errors="replace"))
    return process.stdout


def generated_verification(suite: Path, output: Path):
    rendered = render_verification_json(verify(load_verification_suite(suite)))
    output.write_text(rendered, encoding="utf-8")
    return load_verification_artifact(output), json.loads(rendered)


def finding_rows(artifact):
    return [
        finding
        for check in artifact.checks.values()
        for finding in check.result.findings
        if finding.disposition == "open"
        and finding.applicability == "applicable"
    ]


def finding(artifact, check_id: str, key: str):
    return next(
        row
        for row in artifact.checks[check_id].result.findings
        if row.key == key
    )


def normalized_artifact(document: dict, *, oracle: bool = False) -> dict:
    """Remove only implementation-identity-dependent seal fields."""

    normalized = dict(document)
    normalized.pop("tool", None)
    normalized.pop("sha256", None)
    for key in ("proposal_sha256", "contract_sha256"):
        if key in normalized:
            normalized[key] = "<sealed-reference>"
    if oracle and normalized.get("artifact") == "change-proposal":
        # The immutable 0.10 compiler turns asserted literal evidence into an
        # edit candidate with project="", although its own proposal loader and
        # the public schema require a non-empty project identifier. Native Act
        # deliberately omits that non-actionable candidate and its dependent
        # ownership warning. No valid project-backed candidate is normalized.
        normalized["candidates"] = [
            row for row in normalized["candidates"] if row.get("project")
        ]
        normalized["unknowns"] = [
            row
            for row in normalized["unknowns"]
            if not (
                row.get("code") == "ownership_unasserted"
                and not row.get("scope")
                and row.get("evidence")
                and all(not item.get("project") for item in row["evidence"])
            )
        ]
    return normalized


def assert_equal_artifact(native_document: dict, oracle_document: dict) -> None:
    native_normalized = normalized_artifact(native_document)
    oracle_normalized = normalized_artifact(oracle_document, oracle=True)
    if native_normalized == oracle_normalized:
        return
    native_lines = json.dumps(native_normalized, indent=2, sort_keys=True).splitlines()
    oracle_lines = json.dumps(oracle_normalized, indent=2, sort_keys=True).splitlines()
    difference = "\n".join(
        difflib.unified_diff(
            oracle_lines,
            native_lines,
            fromfile="oracle",
            tofile="native",
            lineterm="",
        )
    )
    raise AssertionError(f"native Act artifact differs from oracle:\n{difference}")


def native_proposal(binary: Path, verification: dict, fingerprint: str) -> dict:
    return json.loads(
        native(
            binary,
            {"verification": verification, "fingerprint": fingerprint},
        )
    )


def native_contract(binary: Path, proposal: dict, **review) -> dict:
    return json.loads(
        native(binary, {"proposal": proposal, "review": review}, "contract")
    )


def native_result(
    binary: Path,
    proposal: dict,
    contract: dict,
    before: dict,
    after: dict,
) -> dict:
    return json.loads(
        native(
            binary,
            {
                "proposal": proposal,
                "contract": contract,
                "before": before,
                "after": after,
            },
            "verify",
        )
    )


def review_for(proposal: dict, preserve_checks=(), selected_candidates=()) -> dict:
    return {
        "objective": "Exercise the exact reviewed transition.",
        "owner": "native-port",
        "rationale": "Permanent native/oracle Act differential.",
        "preserve_checks": list(preserve_checks),
        "selected_candidates": list(selected_candidates),
    }


def seal(document: dict) -> dict:
    sealed = dict(document)
    sealed.pop("sha256", None)
    payload = json.dumps(sealed, sort_keys=True, separators=(",", ":")).encode()
    sealed["sha256"] = hashlib.sha256(payload).hexdigest()
    return sealed


def test_all_proposal_compilers(
    binary: Path, fixture: Path, directory: Path
) -> tuple[object, dict]:
    artifact, verification = generated_verification(
        fixture / "verification.json", directory / "before.verify.json"
    )
    rows = finding_rows(artifact)
    assert rows, "verification fixture has no open applicable findings"
    assertions = set()
    for row in rows:
        oracle_proposal = compile_change_proposal(artifact, row.fingerprint)
        oracle_document = json.loads(render_change_proposal_json(oracle_proposal))
        first_bytes = native(
            binary,
            {"verification": verification, "fingerprint": row.fingerprint},
        )
        second_bytes = native(
            binary,
            {"verification": verification, "fingerprint": row.fingerprint},
        )
        assert first_bytes == second_bytes
        native_document = json.loads(first_bytes)
        assert all(row["project"] for row in native_document["candidates"])
        assert_equal_artifact(native_document, oracle_document)
        native_contract(
            binary,
            native_document,
            **review_for(native_document),
        )
        assertions.add(oracle_proposal.origin_assertion)
    assert assertions == {
        "acyclic",
        "allowed_edges",
        "attestation_equal",
        "forbidden_edges",
        "mapped_set_equal",
        "mapped_values_equal",
        "subset",
    }
    return artifact, verification


def test_wait_lifecycle(
    binary: Path,
    fixture: Path,
    directory: Path,
    artifact,
    before: dict,
) -> None:
    wait = finding(artifact, "PORT-OPS-SET", "WAIT")
    oracle_proposal = compile_change_proposal(artifact, wait.fingerprint)
    proposal = native_proposal(binary, before, wait.fingerprint)
    preserve = ("REFERENCE-OPS-CARDINALITY",)
    review = review_for(proposal, preserve_checks=preserve)
    contract = native_contract(binary, proposal, **review)
    oracle_contract = create_change_contract(
        oracle_proposal,
        objective=review["objective"],
        owner=review["owner"],
        rationale=review["rationale"],
        preserve_checks=preserve,
    )
    assert_equal_artifact(
        contract, json.loads(render_change_contract_json(oracle_contract))
    )

    unchanged = native_result(binary, proposal, contract, before, before)
    oracle_unchanged = verify_change_contract(
        oracle_proposal, oracle_contract, artifact, artifact
    )
    assert_equal_artifact(
        unchanged, json.loads(render_change_result_json(oracle_unchanged))
    )
    assert unchanged["status"] == "missing"

    copied = directory / "wait-success"
    shutil.copytree(fixture, copied)
    header = copied / "subject" / "src" / "core.h"
    header.write_text(
        header.read_text(encoding="utf-8").replace(
            "  PORT_EXTRA,", "  PORT_WAIT,\n  PORT_EXTRA,"
        ),
        encoding="utf-8",
    )
    after_artifact, after = generated_verification(
        copied / "verification.json", directory / "after.verify.json"
    )
    success = native_result(binary, proposal, contract, before, after)
    oracle_success = verify_change_contract(
        oracle_proposal, oracle_contract, artifact, after_artifact
    )
    assert_equal_artifact(success, json.loads(render_change_result_json(oracle_success)))
    assert (success["freshness"], success["status"]) == ("current", "satisfied")

    drift_root = directory / "wait-context-drift"
    shutil.copytree(fixture, drift_root)
    runner = drift_root / "subject" / "runner.py"
    runner.write_text(
        runner.read_text(encoding="utf-8") + "\n# unrelated drift\n",
        encoding="utf-8",
    )
    drift_artifact, drift = generated_verification(
        drift_root / "verification.json", directory / "drift.verify.json"
    )
    drift_result = native_result(binary, proposal, contract, drift, drift)
    oracle_drift = verify_change_contract(
        oracle_proposal, oracle_contract, drift_artifact, drift_artifact
    )
    assert_equal_artifact(
        drift_result, json.loads(render_change_result_json(oracle_drift))
    )
    assert (drift_result["freshness"], drift_result["status"]) == (
        "context_drift",
        "missing",
    )

    stale_root = directory / "wait-stale"
    shutil.copytree(fixture, stale_root)
    stale_header = stale_root / "subject" / "src" / "core.h"
    stale_header.write_text(
        stale_header.read_text(encoding="utf-8").replace(
            "PORT_EXTRA", "PORT_OTHER"
        ),
        encoding="utf-8",
    )
    stale_artifact, stale_verification = generated_verification(
        stale_root / "verification.json", directory / "stale.verify.json"
    )
    stale = native_result(
        binary, proposal, contract, stale_verification, stale_verification
    )
    oracle_stale = verify_change_contract(
        oracle_proposal, oracle_contract, stale_artifact, stale_artifact
    )
    assert_equal_artifact(stale, json.loads(render_change_result_json(oracle_stale)))
    assert stale["status"] == "stale"

    superseded = native_result(binary, proposal, contract, after, after)
    oracle_superseded = verify_change_contract(
        oracle_proposal, oracle_contract, after_artifact, after_artifact
    )
    assert_equal_artifact(
        superseded, json.loads(render_change_result_json(oracle_superseded))
    )
    assert (superseded["freshness"], superseded["status"]) == (
        "superseded",
        "superseded",
    )

    report_envelope = {
        "proposal": proposal,
        "contract": contract,
        "before": before,
        "after": after,
    }
    markdown = native(binary, report_envelope, "markdown").decode()
    assert "# Architecture change result" in markdown
    assert str(directory) not in markdown
    sarif = json.loads(native(binary, report_envelope, "sarif"))
    assert sarif["version"] == "2.1.0"
    junit = ET.fromstring(native(binary, report_envelope, "junit"))
    assert junit.tag == "testsuite"

    tampered = json.loads(json.dumps(proposal))
    tampered["unknowns"][0]["message"] = "tampered"
    rejected = subprocess.run(
        [str(binary), "contract"],
        input=json.dumps({"proposal": tampered, "review": review}).encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert rejected.returncode != 0

    invalid_contract = dict(contract)
    invalid_contract["acknowledged_unknowns"] = []
    invalid_contract = seal(invalid_contract)
    rejected = subprocess.run(
        [str(binary), "verify"],
        input=json.dumps(
            {
                "proposal": proposal,
                "contract": invalid_contract,
                "before": before,
                "after": before,
            }
        ).encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert rejected.returncode != 0


def test_provider_rename(binary: Path, fixture: Path, directory: Path) -> None:
    artifact, before = generated_verification(
        fixture / "provider.verify.json", directory / "provider-before.verify.json"
    )
    row = finding(artifact, "PROVIDER-RENAME", "core_sum")
    oracle_proposal = compile_change_proposal(artifact, row.fingerprint)
    proposal = native_proposal(binary, before, row.fingerprint)
    assert_equal_artifact(
        proposal, json.loads(render_change_proposal_json(oracle_proposal))
    )
    expected_paths = {
        "Makefile",
        "src/core.h",
        "src/core.c",
        "py/api.py",
        "py/test_api.py",
        "js/runtime.js",
        "js/test_api.js",
    }
    assert {candidate["path"] for candidate in proposal["candidates"]} == expected_paths
    selected = tuple(candidate["id"] for candidate in proposal["candidates"])
    preserve = ("PROVIDER-TEST-ROUTES",)
    review = review_for(
        proposal, preserve_checks=preserve, selected_candidates=selected
    )
    contract = native_contract(binary, proposal, **review)
    oracle_contract = create_change_contract(
        oracle_proposal,
        objective=review["objective"],
        owner=review["owner"],
        rationale=review["rationale"],
        preserve_checks=preserve,
        selected_candidates=selected,
    )
    assert_equal_artifact(
        contract, json.loads(render_change_contract_json(oracle_contract))
    )

    copied = directory / "provider-success"
    shutil.copytree(fixture, copied)
    for relative in sorted(expected_paths):
        path = copied / "subject" / relative
        path.write_text(
            path.read_text(encoding="utf-8").replace("core_add", "core_sum"),
            encoding="utf-8",
        )
    after_artifact, after = generated_verification(
        copied / "provider.verify.json", directory / "provider-after.verify.json"
    )
    result = native_result(binary, proposal, contract, before, after)
    oracle_result = verify_change_contract(
        oracle_proposal, oracle_contract, artifact, after_artifact
    )
    assert_equal_artifact(result, json.loads(render_change_result_json(oracle_result)))
    assert result["status"] == "satisfied"


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_act_results.py NATIVE_ACT REPOSITORY VERIFY_FIXTURE"
        )
    binary = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    oracle_root = Path(__import__("archbird").__file__).resolve().parents[1]
    provider_fixture = oracle_root / "tests" / "fixtures" / "act" / "provider"
    with tempfile.TemporaryDirectory(
        prefix="act-results-", dir=repository / "build"
    ) as temporary:
        directory = Path(temporary)
        artifact, before = test_all_proposal_compilers(binary, fixture, directory)
        test_wait_lifecycle(binary, fixture, directory, artifact, before)
        test_provider_rename(binary, provider_fixture, directory)
    print("native Act proposal/contract/result differentials passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
