#!/usr/bin/env python3
"""Differential native OKF publication against a maintainer reference."""

from __future__ import annotations

import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def canonical(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")


def native_expected_page(path: str, text: str) -> str:
    """Project pre-native OKF pages onto explicit provenance wording."""

    if not path.startswith("verification/projects/"):
        return text
    text = text.replace(
        "| Project | Revision | Profile | Input SHA-256 | Config SHA-256 |\n"
        "| --- | --- | --- | --- | --- |\n",
        "| Project | Declared revision (asserted) | Source lock | Profile | "
        "Input SHA-256 | Config SHA-256 |\n"
        "| --- | --- | --- | --- | --- | --- |\n",
        1,
    )
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if not line.startswith("| <code>") or line.count(" | ") != 4:
            continue
        cells = line.rstrip("\n").split(" | ")
        cells.insert(2, "<code>not_declared</code>")
        lines[index] = " | ".join(cells) + "\n"
        break
    return "".join(lines)


def run_native(binary: Path, paths: tuple[Path | None, ...]) -> bytes:
    result = subprocess.run(
        [str(binary), *(str(path) if path else "-" for path in paths)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise AssertionError(result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def expect_failure(binary: Path, paths: tuple[Path | None, ...], message: str) -> None:
    result = subprocess.run(
        [str(binary), *(str(path) if path else "-" for path in paths)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if not result.returncode or message not in result.stderr.decode(
        "utf-8", errors="replace"
    ):
        raise AssertionError(
            f"native publisher did not reject {message!r}: "
            f"rc={result.returncode}, stderr={result.stderr!r}"
        )


def write_oracle_artifacts(root: Path, oracle: Path):
    sys.path.insert(0, str(oracle))
    from archbird import __version__, implementation_digest
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
    from archbird.interchange.okf.writer import build_okf_bundle
    from archbird.map.analyze import analyze
    from archbird.map.config import load_config
    from archbird.map.render import render_json
    from archbird.verify.config import load_verification_suite
    from archbird.verify.engine import verify
    from archbird.verify.render import render_verification_json

    fixture = oracle / "tests" / "fixtures" / "verification"
    map_path = root / "subject.map.json"
    map_path.write_text(
        render_json(
            analyze(
                load_config(
                    fixture / "subject" / "archbird.json",
                    root_override=fixture / "subject",
                )
            )
        ),
        encoding="utf-8",
    )
    verification_path = root / "before.verify.json"
    verification_path.write_text(
        render_verification_json(
            verify(load_verification_suite(fixture / "verification.json"))
        ),
        encoding="utf-8",
    )
    before = load_verification_artifact(verification_path)
    finding = next(
        row
        for row in before.checks["PORT-OPS-SET"].result.findings
        if row.key == "WAIT"
    )
    proposal = compile_change_proposal(before, finding.fingerprint)
    proposal_path = root / "proposal.json"
    proposal_path.write_text(render_change_proposal_json(proposal), encoding="utf-8")
    contract = create_change_contract(
        proposal,
        objective="Port WAIT.",
        owner="compiler-core",
        rationale="OKF integration fixture.",
        preserve_checks=("REFERENCE-OPS-CARDINALITY",),
    )
    contract_path = root / "contract.json"
    contract_path.write_text(render_change_contract_json(contract), encoding="utf-8")
    change_result = verify_change_contract(proposal, contract, before, before)
    result_path = root / "result.json"
    result_path.write_text(
        render_change_result_json(change_result), encoding="utf-8"
    )
    bundle = build_okf_bundle(
        map_path,
        verification_path=verification_path,
        proposal_path=proposal_path,
        contract_path=contract_path,
        result_path=result_path,
    )
    oracle_tool = {
        "implementation_sha256": implementation_digest(),
        "name": "archbird",
        "version": __version__,
    }
    return (
        (map_path, verification_path, proposal_path, contract_path, result_path),
        bundle,
        oracle_tool,
    )


def assert_parity(native_raw: bytes, oracle_bundle, oracle_tool: dict[str, str]) -> None:
    native = json.loads(native_raw)
    if native.get("artifact") != "okf-output-bundle":
        raise AssertionError("native publisher did not emit an OKF output bundle")
    native_files = {row["path"]: row["text"] for row in native["files"]}
    oracle_files = oracle_bundle.as_dict()
    if set(native_files) != set(oracle_files):
        raise AssertionError("native and oracle OKF inventories differ")
    native_identity = canonical(native["tool"]).decode("utf-8")
    oracle_identity = canonical(oracle_tool).decode("utf-8")
    for path, expected in oracle_files.items():
        if path == "provenance/integrity.md":
            continue
        expected = native_expected_page(path, expected)
        actual = native_files[path].replace(native_identity, oracle_identity)
        if actual != expected:
            raise AssertionError(
                f"native OKF page differs from oracle: {path}\n"
                + "".join(
                    difflib.unified_diff(
                        expected.splitlines(keepends=True),
                        actual.splitlines(keepends=True),
                        fromfile="oracle",
                        tofile="native",
                    )
                )
            )
    normalized_entries = [
        (
            path,
            hashlib.sha256(
                text.replace(native_identity, oracle_identity).encode("utf-8")
            ).hexdigest(),
        )
        for path, text in sorted(native_files.items())
        if path != "provenance/integrity.md"
    ]
    normalized_content = hashlib.sha256(canonical(normalized_entries)).hexdigest()
    expected_entries = [
        (path, hashlib.sha256(native_expected_page(path, text).encode("utf-8")).hexdigest())
        for path, text in sorted(oracle_files.items())
        if path != "provenance/integrity.md"
    ]
    expected_content = hashlib.sha256(canonical(expected_entries)).hexdigest()
    if normalized_content != expected_content:
        raise AssertionError("normalized native OKF integrity differs from oracle")


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    oracle = Path(sys.argv[2]).resolve()
    build = Path(sys.argv[3]).resolve()
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=build) as directory:
        root = Path(directory)
        paths, bundle, oracle_tool = write_oracle_artifacts(root, oracle)
        first = run_native(binary, (*paths, None))
        second = run_native(binary, (*paths, None))
        if first != second:
            raise AssertionError("native OKF publication is nondeterministic")
        assert_parity(first, bundle, oracle_tool)

        map_only = run_native(binary, (paths[0], None, None, None, None, None))
        if len(json.loads(map_only)["files"]) >= len(json.loads(first)["files"]):
            raise AssertionError("map-only OKF publication did not omit optional concepts")

        expect_failure(
            binary,
            (paths[0], None, paths[2], None, None, None),
            "proposal requires verification",
        )
        tampered = json.loads(paths[3].read_text(encoding="utf-8"))
        tampered["owner"] = "tampered"
        tampered_path = root / "tampered-contract.json"
        tampered_path.write_bytes(canonical(tampered))
        expect_failure(
            binary,
            (paths[0], paths[1], paths[2], tampered_path, None, None),
            "invalid or unsealed canonical architecture change contract",
        )

        unicode_map = json.loads(paths[0].read_text(encoding="utf-8"))
        unicode_map["project"] = "Straße"
        unicode_path = root / "unicode.map.json"
        unicode_path.write_bytes(canonical(unicode_map))
        expect_failure(
            binary,
            (unicode_path, None, None, None, None, None),
            "normalization evidence",
        )
        normalization = root / "normalization.json"
        normalization.write_bytes(
            canonical(
                {
                    "artifact": "okf-text-normalization",
                    "rows": [
                        {
                            "casefold": "strasse",
                            "slug_ascii": "Strae",
                            "text": "Straße",
                        }
                    ],
                    "schema_version": 1,
                }
            )
        )
        unicode_output = run_native(
            binary, (unicode_path, None, None, None, None, normalization)
        )
        if json.loads(unicode_output)["project"] != "Straße":
            raise AssertionError("native OKF publication lost Unicode project identity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
