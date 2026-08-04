#!/usr/bin/env python3
"""Assemble the exact-archive release verdict from passed gate reports."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


EXPECTED_REPORTS = {
    "browser-wasm": "archbird-browser-release-conformance",
    "node-cli-native": "archbird-node-cli-release-conformance",
    "node-cli-wasm": "archbird-node-cli-release-conformance",
    "node-native-prebuilt": "archbird-node-release-conformance",
    "node-native-rebuilt": "archbird-node-release-conformance",
    "node-readme": "archbird-node-readme-conformance",
    "python-sdist-contract": "archbird-python-native-contract",
    "python-sdist-runtime": "archbird-python-release-conformance",
    "python-wheel-contract": "archbird-python-native-contract",
    "python-wheel-runtime": "archbird-python-release-conformance",
    "python-sdist-readme": "archbird-python-readme-conformance",
    "python-wheel-readme": "archbird-python-readme-conformance",
}

EXPECTED_OPERATIONS = {
    "browser-wasm": {
        "freshness",
        "map",
        "projection",
        "semantic-index",
    },
    "node-cli-native": {
        "act",
        "apply",
        "map",
        "path",
        "plan",
        "query",
        "source",
        "support",
        "test-observations",
        "verify",
    },
    "node-cli-wasm": {
        "act",
        "apply",
        "map",
        "path",
        "plan",
        "query",
        "source",
        "support",
        "test-observations",
        "verify",
    },
    "node-native-prebuilt": {
        "dispose",
        "freshness",
        "graph",
        "map",
        "query",
    },
    "node-native-rebuilt": {
        "dispose",
        "freshness",
        "graph",
        "map",
        "query",
    },
    "node-readme": {
        "api",
        "config",
        "freshness",
        "graph",
        "impact",
        "map",
        "path",
        "query",
        "search",
        "source",
        "support",
        "verify",
    },
    "python-sdist-runtime": {
        "configuration",
        "constraints",
        "freshness",
        "graph",
        "map-buffered",
        "map-cli-json-no-cache",
        "map-streamed",
        "map-streamed-short-write",
        "map-streamed-sink-exception",
        "projection",
        "query",
        "query-plan",
        "sarif",
    },
    "python-wheel-runtime": {
        "configuration",
        "constraints",
        "freshness",
        "graph",
        "map-buffered",
        "map-cli-json-no-cache",
        "map-streamed",
        "map-streamed-short-write",
        "map-streamed-sink-exception",
        "projection",
        "query",
        "query-plan",
        "sarif",
    },
    "python-sdist-readme": {
        "api",
        "config",
        "freshness",
        "graph",
        "impact",
        "map",
        "path",
        "query",
        "search",
        "source",
        "verify",
    },
    "python-wheel-readme": {
        "api",
        "config",
        "freshness",
        "graph",
        "impact",
        "map",
        "path",
        "query",
        "search",
        "source",
        "verify",
    },
}

EXPECTED_ENGINES = {
    "browser-wasm": "wasm",
    "node-cli-native": "native",
    "node-cli-wasm": "wasm",
    "node-native-prebuilt": "native",
    "node-native-rebuilt": "native",
}

PARITY_KEYS = {"act", "map", "plan", "query", "verification"}


def _load(path: Path) -> tuple[bytes, dict[str, object]]:
    data = path.read_bytes()
    document = json.loads(data)
    if not isinstance(document, dict):
        raise ValueError(f"report is not a JSON object: {path}")
    return data, document


def _sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _validate_contract(label: str, document: dict[str, object]) -> None:
    compiled = document.get("compiled_operations")
    ctypes = document.get("ctypes_operations")
    wrapper_calls = document.get("wrapper_calls")
    if (
        not isinstance(compiled, list)
        or not compiled
        or compiled != ctypes
        or not isinstance(wrapper_calls, list)
        or not set(wrapper_calls).issubset(set(compiled))
    ):
        raise AssertionError(f"{label}: native contract operation inventory differs")
    for name in (
        "extra_compiled_operations",
        "missing_compiled_calls",
        "missing_compiled_operations",
        "missing_compiled_references",
        "missing_ctypes_calls",
    ):
        if document.get(name) != []:
            raise AssertionError(f"{label}: native contract reports {name}")


def _validate_report(label: str, document: dict[str, object]) -> None:
    if not _sha256(document.get("implementation_sha256")):
        raise AssertionError(f"{label}: implementation identity is invalid")
    if label.endswith("-contract"):
        _validate_contract(label, document)
        return
    operations = document.get("operations")
    if (
        not isinstance(operations, list)
        or len(operations) != len(set(operations))
        or set(operations) != EXPECTED_OPERATIONS[label]
    ):
        raise AssertionError(f"{label}: conformance operation inventory differs")
    expected_documents = {
        "node-readme": ["README.md", "js/README.md"],
        "python-sdist-readme": ["README.md", "py/README.md"],
        "python-wheel-readme": ["README.md", "py/README.md"],
    }.get(label)
    if (
        expected_documents is not None
        and document.get("documents") != expected_documents
    ):
        raise AssertionError(f"{label}: README inventory differs")
    if expected_documents is not None:
        surfaces = document.get("surfaces")
        if not isinstance(surfaces, dict) or not surfaces:
            raise AssertionError(f"{label}: public surface inventory is absent")
        for name, values in surfaces.items():
            if (
                not isinstance(name, str)
                or not name
                or not isinstance(values, list)
                or not values
                or values != sorted(set(values))
                or not all(isinstance(value, str) and value for value in values)
            ):
                raise AssertionError(
                    f"{label}: public surface inventory {name!r} is invalid"
                )
        examples = document.get("examples")
        if (
            not isinstance(examples, list)
            or not examples
            or [row.get("id") for row in examples]
            != sorted({row.get("id") for row in examples})
        ):
            raise AssertionError(f"{label}: executable example inventory is invalid")
        for row in examples:
            if (
                not isinstance(row, dict)
                or row.get("category") not in {"tested-pass", "tested-failure"}
                or not isinstance(row.get("covers"), list)
                or not row["covers"]
                or not set(row["covers"]).issubset(set(operations))
            ):
                raise AssertionError(
                    f"{label}: executable example coverage is invalid"
                )
    expected_engine = EXPECTED_ENGINES.get(label)
    if expected_engine is not None and document.get("engine") != expected_engine:
        raise AssertionError(
            f"{label}: expected {expected_engine} engine, "
            f"got {document.get('engine')!r}"
        )
    if label in {
        "python-sdist-runtime",
        "python-wheel-runtime",
        "node-native-prebuilt",
        "node-native-rebuilt",
    } and not _sha256(document.get("map_sha256")):
        raise AssertionError(f"{label}: canonical Map identity is invalid")
    if label in {"node-cli-native", "node-cli-wasm"}:
        parity = document.get("parity")
        if (
            not isinstance(parity, dict)
            or set(parity) != PARITY_KEYS
            or not all(_sha256(value) for value in parity.values())
        ):
            raise AssertionError(f"{label}: parity ledger is invalid")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provenance", required=True)
    parser.add_argument("--transition", required=True)
    parser.add_argument("--report", action="append", default=[])
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    report_paths: dict[str, Path] = {}
    for value in args.report:
        label, separator, raw_path = value.partition("=")
        if not separator or not label or not raw_path:
            raise SystemExit("--report must be LABEL=PATH")
        if label in report_paths:
            raise SystemExit(f"duplicate release report label: {label}")
        report_paths[label] = Path(raw_path).resolve()
    if set(report_paths) != set(EXPECTED_REPORTS):
        raise AssertionError(
            "release conformance report inventory differs: "
            f"missing={sorted(set(EXPECTED_REPORTS) - set(report_paths))!r} "
            f"extra={sorted(set(report_paths) - set(EXPECTED_REPORTS))!r}"
        )

    provenance_data, provenance = _load(Path(args.provenance).resolve())
    if (
        provenance.get("artifact") != "archbird-release-provenance-check"
        or provenance.get("failures") != []
    ):
        raise AssertionError("release provenance did not pass")
    version = provenance.get("version")
    transition_data, transition = _load(Path(args.transition).resolve())
    if (
        transition.get("artifact") != "archbird-release-transition"
        or transition.get("failures") != []
        or transition.get("version") != version
        or transition.get("source_commit") != provenance.get("source_commit")
        or transition.get("source_tree") != provenance.get("source_tree")
        or transition.get("tag") != provenance.get("tag")
    ):
        raise AssertionError("release self-transition did not pass")
    for name in ("before_map", "after_map"):
        identity = transition.get(name)
        if (
            not isinstance(identity, dict)
            or not _sha256(identity.get("input_sha256"))
            or not _sha256(identity.get("sha256"))
        ):
            raise AssertionError(f"release self-transition {name} is invalid")
    for name in ("before_freshness", "after_freshness"):
        freshness = transition.get(name)
        if (
            not isinstance(freshness, dict)
            or freshness.get("status") != "current"
            or not _sha256(freshness.get("sha256"))
        ):
            raise AssertionError(f"release self-transition {name} is invalid")
    for name in (
        "diff_sha256",
        "expected_changes_sha256",
        "verification_result_sha256",
        "verification_sha256",
    ):
        if not _sha256(transition.get(name)):
            raise AssertionError(
                f"release self-transition {name} is invalid"
            )
    rows = []
    reports: dict[str, dict[str, object]] = {}
    for label in sorted(report_paths):
        data, document = _load(report_paths[label])
        expected_artifact = EXPECTED_REPORTS[label]
        if document.get("artifact") != expected_artifact:
            raise AssertionError(
                f"{label}: expected {expected_artifact}, "
                f"got {document.get('artifact')!r}"
            )
        if document.get("version") != version:
            raise AssertionError(f"{label}: report version differs")
        _validate_report(label, document)
        reports[label] = document
        rows.append(
            {
                "artifact": expected_artifact,
                "label": label,
                "path": report_paths[label].name,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    implementation_identities = {
        document["implementation_sha256"] for document in reports.values()
    }
    if len(implementation_identities) != 1:
        raise AssertionError(
            "release reports used different core implementation identities"
        )
    if (
        reports["python-wheel-runtime"]["map_sha256"]
        != reports["python-sdist-runtime"]["map_sha256"]
    ):
        raise AssertionError("wheel and sdist-rebuilt wheel Maps differ")
    if (
        reports["node-native-prebuilt"]["map_sha256"]
        != reports["node-native-rebuilt"]["map_sha256"]
    ):
        raise AssertionError("prebuilt and rebuilt Node native Maps differ")
    if (
        reports["node-cli-native"]["parity"]
        != reports["node-cli-wasm"]["parity"]
    ):
        raise AssertionError("Node native and Wasm canonical results differ")
    if (
        reports["python-wheel-readme"]["examples"]
        != reports["python-sdist-readme"]["examples"]
    ):
        raise AssertionError("wheel and sdist README contracts differ")
    coverage: dict[tuple[str, str], set[str]] = {}
    for label, document in reports.items():
        for operation in document.get("operations", []):
            coverage.setdefault(("observed-operation", operation), set()).add(label)
        for surface, names in document.get("surfaces", {}).items():
            for name in names:
                coverage.setdefault((surface, name), set()).add(label)
        for example in document.get("examples", []):
            coverage.setdefault(
                ("documented-example", example["id"]), set()
            ).add(label)
    coverage_rows = [
        {
            "evidence": evidence,
            "name": name,
            "reports": sorted(labels),
        }
        for (evidence, name), labels in sorted(coverage.items())
    ]
    if not coverage_rows:
        raise AssertionError("release contract coverage ledger is empty")

    provenance_archives = provenance.get("archives")
    if not isinstance(provenance_archives, list) or len(provenance_archives) != 3:
        raise AssertionError("release provenance archive inventory differs")
    archives = []
    for row in provenance_archives:
        if not isinstance(row, dict):
            raise AssertionError("release provenance archive row is invalid")
        raw_path = str(row.get("path", ""))
        sha256 = row.get("sha256")
        content_hashes = {
            name: row.get(name)
            for name in (
                "c_source_manifest_sha256",
                "package_metadata_sha256",
                "readme_sha256",
                "release_provenance_sha256",
                "schema_manifest_sha256",
            )
            if name in row
        }
        if (
            not isinstance(row.get("bytes"), int)
            or row["bytes"] <= 0
            or not _sha256(sha256)
            or not content_hashes
            or not all(_sha256(value) for value in content_hashes.values())
            or "readme_sha256" not in content_hashes
            or "release_provenance_sha256" not in content_hashes
            or "schema_manifest_sha256" not in content_hashes
        ):
            raise AssertionError("release provenance archive measurement is invalid")
        archives.append(
            {
                "bytes": row.get("bytes"),
                "content": content_hashes,
                "path": Path(raw_path).name,
                "sha256": sha256,
            }
        )
    archive_names = [row["path"] for row in archives]
    if (
        sum(name.endswith(".whl") for name in archive_names) != 1
        or sum(name.endswith(".tar.gz") for name in archive_names) != 1
        or sum(name.endswith(".tgz") for name in archive_names) != 1
    ):
        raise AssertionError("release provenance archive kinds differ")
    attestation = {
        "archives": archives,
        "artifact": "archbird-release-attestation",
        "conformance": rows,
        "coverage": coverage_rows,
        "implementation_sha256": next(iter(implementation_identities)),
        "provenance_sha256": hashlib.sha256(provenance_data).hexdigest(),
        "schema_version": 1,
        "source_commit": provenance.get("source_commit"),
        "source_tree": provenance.get("source_tree"),
        "tag": provenance.get("tag"),
        "transition": {
            "after_map": transition.get("after_map"),
            "after_freshness": transition.get("after_freshness"),
            "before_commit": transition.get("before_commit"),
            "before_map": transition.get("before_map"),
            "before_ref": transition.get("before_ref"),
            "before_freshness": transition.get("before_freshness"),
            "diff_sha256": transition.get("diff_sha256"),
            "expected_changes_sha256": transition.get(
                "expected_changes_sha256"
            ),
            "sha256": hashlib.sha256(transition_data).hexdigest(),
            "verification_result_sha256": transition.get(
                "verification_result_sha256"
            ),
            "verification_sha256": transition.get("verification_sha256"),
        },
        "version": version,
    }
    output = Path(args.output)
    output.write_text(
        json.dumps(attestation, ensure_ascii=True, indent=2, sort_keys=True)
        + "\n",
        encoding="utf-8",
    )
    print(
        f"release attestation passed: version={version} "
        f"archives={len(archives)} reports={len(rows)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
