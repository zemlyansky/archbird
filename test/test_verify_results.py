#!/usr/bin/env python3
"""Differential public native verification results for every static check."""

from __future__ import annotations

import importlib.util
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from archbird.map.render import render_json
from archbird.verify.config import load_verification_suite
from archbird.verify.engine import apply_baseline, verify
from archbird.verify.render import verification_dict
from archbird.verify.render import (
    render_verification_junit,
    render_verification_markdown,
    render_verification_sarif,
)
import xml.etree.ElementTree as ET


def load_provider(path: Path):
    spec = importlib.util.spec_from_file_location(
        "archbird_native_verification_result_provider", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load provider {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def static_suite(fixture: Path, directory: Path) -> tuple[Path, dict]:
    document = json.loads(
        (fixture / "verification.json").read_text(encoding="utf-8")
    )
    document["attestations"] = {}
    document["checks"] = [
        row for row in document["checks"] if row["assert"] != "attestation_equal"
    ]
    document["extractors"].update(
        {
            "matrix.a": {"kind": "literal_set", "values": ["A"]},
            "matrix.ab": {"kind": "literal_set", "values": ["A", "B"]},
            "matrix.ax": {"kind": "literal_set", "values": ["A", "X"]},
            "matrix.empty": {"kind": "literal_set", "values": []},
            "matrix.values.expected": {
                "kind": "literal_values",
                "values": {"A": 1, "B": 2},
            },
            "matrix.values.actual": {
                "kind": "literal_values",
                "values": {"A": 1, "B": 3},
            },
            "matrix.edge.one": {
                "kind": "literal_relation",
                "rows": [{"source": "left", "target": "right"}],
            },
            "matrix.edge.other": {
                "kind": "literal_relation",
                "rows": [{"source": "other", "kind": "call", "target": "end"}],
            },
            "matrix.relation.empty": {"kind": "literal_relation", "rows": []},
        }
    )
    document["mappings"]["collision"] = {"actual_to_expected": {"X": "A"}}
    document["checks"].extend(
        [
            {
                "id": "MATRIX-SET-PASS",
                "assert": "set_equal",
                "expected": "matrix.a",
                "actual": "matrix.a",
                "owner": "test",
                "rationale": "Exercise an exact set pass.",
            },
            {
                "id": "MATRIX-SET-FAIL",
                "assert": "set_equal",
                "expected": "matrix.a",
                "actual": "matrix.ab",
                "owner": "test",
                "rationale": "Exercise an unmapped set difference.",
            },
            {
                "id": "MATRIX-VALUES-FAIL",
                "assert": "values_equal",
                "expected": "matrix.values.expected",
                "actual": "matrix.values.actual",
                "owner": "test",
                "rationale": "Exercise an unmapped value difference.",
            },
            {
                "id": "MATRIX-SHAPE-UNKNOWN",
                "assert": "set_equal",
                "expected": "matrix.edge.one",
                "actual": "matrix.a",
                "owner": "test",
                "rationale": "Reject incompatible fact shapes honestly.",
            },
            {
                "id": "MATRIX-MAPPING-COLLISION",
                "assert": "mapped_set_equal",
                "expected": "matrix.a",
                "actual": "matrix.ax",
                "mapping": "collision",
                "owner": "test",
                "rationale": "Keep mapping collisions unknown rather than choosing.",
            },
            {
                "id": "MATRIX-REQUIRED-MISSING",
                "assert": "required_edges",
                "expected": "matrix.edge.other",
                "actual": "matrix.edge.one",
                "owner": "test",
                "rationale": "Exercise a missing required relation.",
            },
            {
                "id": "MATRIX-FORBIDDEN-PASS",
                "assert": "forbidden_edges",
                "expected": "matrix.edge.other",
                "actual": "matrix.edge.one",
                "owner": "test",
                "rationale": "Exercise a clean forbidden relation check.",
            },
            {
                "id": "MATRIX-ALLOWED-PASS",
                "assert": "allowed_edges",
                "expected": "matrix.edge.one",
                "actual": "matrix.edge.one",
                "owner": "test",
                "rationale": "Exercise a clean allowed relation check.",
            },
            {
                "id": "MATRIX-ACYCLIC-PASS",
                "assert": "acyclic",
                "actual": "matrix.edge.one",
                "owner": "test",
                "rationale": "Exercise an acyclic relation.",
            },
            {
                "id": "MATRIX-CARDINALITY-MIN",
                "assert": "cardinality",
                "actual": "matrix.a",
                "min": 2,
                "owner": "test",
                "rationale": "Exercise a minimum cardinality failure.",
            },
            {
                "id": "MATRIX-CARDINALITY-MAX",
                "assert": "cardinality",
                "actual": "matrix.ab",
                "max": 1,
                "owner": "test",
                "rationale": "Exercise a maximum cardinality failure.",
            },
            {
                "id": "MATRIX-ROUTES-MISSING",
                "assert": "min_test_routes",
                "actual": "matrix.relation.empty",
                "min": 1,
                "required_routes": ["missing.c"],
                "owner": "test",
                "rationale": "Exercise required-route and route-count failures.",
            },
        ]
    )
    for name, row in document["projects"].items():
        row["config"] = Path(
            os.path.relpath(fixture / name / "archbird.json", directory)
        ).as_posix()
    path = directory / "static.verify.json"
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return path, document


def full_suite(fixture: Path, directory: Path) -> tuple[Path, dict]:
    document = json.loads(
        (fixture / "verification.json").read_text(encoding="utf-8")
    )
    for name, row in document["projects"].items():
        row["config"] = Path(
            os.path.relpath(fixture / name / "archbird.json", directory)
        ).as_posix()
    for row in document["attestations"].values():
        row["path"] = Path(
            os.path.relpath(fixture / row["path"], directory)
        ).as_posix()
    path = directory / "full.verify.json"
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return path, document


def attestation_variant_suite(
    fixture: Path,
    directory: Path,
    label: str,
    mutate,
) -> tuple[Path, dict]:
    document = json.loads(
        (fixture / "verification.json").read_text(encoding="utf-8")
    )
    for name, row in document["projects"].items():
        row["config"] = Path(
            os.path.relpath(fixture / name / "archbird.json", directory)
        ).as_posix()
    attestation_documents = {
        name: json.loads((fixture / row["path"]).read_text(encoding="utf-8"))
        for name, row in document["attestations"].items()
    }
    mutate(attestation_documents)
    for name, row in document["attestations"].items():
        attestation_path = directory / f"{label}.{name}.attestation.json"
        attestation_path.write_text(
            json.dumps(
                attestation_documents[name],
                indent=2,
                sort_keys=True,
                ensure_ascii=False,
            )
            + "\n",
            encoding="utf-8",
        )
        row["path"] = attestation_path.name
    suite_path = directory / f"{label}.verify.json"
    suite_path.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return suite_path, document


def evidence_slice(document: dict) -> str:
    rows = [
        {"path": row["path"], "role": row["role"], "sha256": row["sha256"]}
        for row in sorted(
            document["producer"]["evidence"],
            key=lambda row: (row["role"], row["path"], row["sha256"]),
        )
    ]
    return hashlib.sha256(
        json.dumps(
            rows,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=False,
        ).encode("utf-8")
    ).hexdigest()


def build_envelope(
    repository: Path,
    fixture: Path,
    suite_path: Path,
    document: dict,
    baseline_document: dict | None = None,
) -> tuple[bytes, dict]:
    suite = load_verification_suite(suite_path)
    data = verify(suite)
    if baseline_document is not None:
        baseline_path = suite_path.parent / "input.baseline.json"
        baseline_path.write_text(
            json.dumps(
                baseline_document, indent=2, sort_keys=True, ensure_ascii=False
            )
            + "\n",
            encoding="utf-8",
        )
        apply_baseline(data, baseline_path)
    provider = load_provider(repository / "py/archbird/providers/verification.py")
    projects = []
    provided = []
    attestation_documents = {
        name: json.loads((suite_path.parent / row["path"]).read_text(encoding="utf-8"))
        for name, row in document.get("attestations", {}).items()
    }
    for name, project in data.projects.items():
        root = fixture / name
        paths = sorted(
            {
                row["path"]
                for row in document["extractors"].values()
                if row.get("project") == name and "path" in row
            }
        )
        sources = [
            {
                "path": path,
                "text": (root / path).read_text(encoding="utf-8"),
            }
            for path in paths
        ]
        source_paths = set(paths)
        for attestation_name, attestation_spec in document.get(
            "attestations", {}
        ).items():
            if attestation_spec["project"] != name:
                continue
            for evidence in attestation_documents[attestation_name]["producer"][
                "evidence"
            ]:
                evidence_path = evidence["path"]
                if evidence_path in source_paths:
                    continue
                sources.append(
                    {
                        "path": evidence_path,
                        "sha256": hashlib.sha256(
                            (root / evidence_path).read_bytes()
                        ).hexdigest(),
                    }
                )
                source_paths.add(evidence_path)
        sources.sort(key=lambda row: row["path"])
        projects.append(
            {
                "name": name,
                "map": json.loads(render_json(project.data)),
                "sources": sources,
            }
        )
        for fact_name, row in sorted(document["extractors"].items()):
            if row.get("project") != name or row["kind"] not in {
                "python_enum",
                "python_set",
            }:
                continue
            path = row["path"]
            provided.append(
                provider.python_verification_fact(
                    name=fact_name,
                    spec=row,
                    project=name,
                    path=path,
                    text=(root / path).read_text(encoding="utf-8"),
                )
            )
    envelope = {
        "suite": document,
        "input": {
            "schema_version": 1,
            "artifact": "verification-input",
            "suite_path": suite_path.name,
            "projects": projects,
            "provided_facts": provided,
            "attestations": [
                {
                    "name": name,
                    "path": row["path"],
                    "document": attestation_documents[name],
                }
                for name, row in sorted(document.get("attestations", {}).items())
            ],
            "baseline": baseline_document,
        },
    }
    encoded = json.dumps(
        envelope,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")
    # Normalize tuples used by the in-memory oracle model to their public JSON
    # representation before comparing them with decoded native JSON.
    expected = json.loads(
        json.dumps(verification_dict(data), ensure_ascii=False, allow_nan=False)
    )
    # Native verification makes asserted revision metadata and the absence of
    # a byte lock explicit; the immutable oracle predates those provenance
    # fields. The coverage labels likewise prevent the numeric sum from being
    # mistaken for code or behavioral coverage.
    for project in expected["projects"]:
        project["revision_provenance"] = "asserted"
        project["source_lock"] = {"files": [], "state": "not_declared"}
    expected["summary"]["coverage_aggregation"] = (
        "sum_of_unique_keys_per_check"
    )
    expected["summary"]["coverage_unit"] = "check_fact_or_route_key"
    return encoded, expected


def run(
    executable: Path, encoded: bytes, *arguments: str
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(executable), *arguments],
        input=encoded,
        capture_output=True,
        check=False,
    )


def xml_projection(element: ET.Element):
    text = element.text or ""
    if not text.strip():
        text = ""
    return (
        element.tag,
        tuple(element.attrib.items()),
        text,
        tuple(xml_projection(child) for child in element),
    )


def native_markdown_projection(report: str) -> str:
    """Project the immutable oracle onto native provenance labels."""

    report = re.sub(
        r"^coverage=([0-9]+) regressions=([0-9]+)$",
        r"coverage_keys=\1 unit=check_fact_or_route_key "
        r"aggregation=sum_of_unique_keys_per_check regressions=\2",
        report,
        flags=re.MULTILINE,
    )
    return re.sub(
        r"^([^:\n]+: project=[^ ]+) revision=([^ ]+) "
        r"(profile=[^\n]+ inputs=[^\n]+)$",
        r"\1 declared_revision=\2 revision_provenance=asserted \3 "
        r"source_lock=not_declared",
        report,
        flags=re.MULTILINE,
    )


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_verify_results.py CLI REPO FIXTURE")
    executable = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    with tempfile.TemporaryDirectory(dir=repository / "build") as raw_directory:
        directory = Path(raw_directory)
        suite_path, document = static_suite(fixture, directory)
        encoded, expected = build_envelope(
            repository, fixture, suite_path, document
        )
        first = run(executable, encoded)
        if first.returncode:
            raise AssertionError(first.stderr.decode("utf-8", errors="replace"))
        second = run(executable, encoded)
        if second.returncode or second.stdout != first.stdout:
            raise AssertionError("native verification result is not deterministic")
        actual = json.loads(first.stdout)
        tool = actual.pop("tool")
        expected.pop("tool")
        if tool.get("name") != "archbird" or tool.get("version") != "0.0.1":
            raise AssertionError(f"invalid native tool identity: {tool}")
        if not re.fullmatch(r"[0-9a-f]{64}", tool.get("implementation_sha256", "")):
            raise AssertionError(f"invalid native implementation identity: {tool}")
        if actual != expected:
            mismatches = [
                key
                for key in sorted(set(actual) | set(expected))
                if actual.get(key) != expected.get(key)
            ]
            details = {
                key: {"expected": expected.get(key), "actual": actual.get(key)}
                for key in mismatches
            }
            raise AssertionError(
                "native/oracle verification result mismatch: "
                + json.dumps(details, indent=2, ensure_ascii=False)
            )

        blocking_rows = [
            {
                "fingerprint": finding["fingerprint"],
                "check": check["id"],
                "comparison": finding["comparison"],
                "key": finding["key"],
            }
            for check in expected["checks"]
            if check["severity"] == "error"
            for finding in check["findings"]
            if finding["applicability"] == "applicable"
            and finding["disposition"] == "open"
            and (
                finding["comparison"] != "equal"
                or finding["evidence_state"] in {"unknown", "stale"}
            )
        ]
        coverage = {
            check["id"]: list(check["coverage"])
            for check in expected["checks"]
        }
        frozen = {
            "schema_version": 1,
            "artifact": "verification-baseline",
            "suite": expected["suite"]["name"],
            "suite_sha256": expected["suite"]["sha256"],
            "tool": {
                "name": "archbird",
                "version": "fixture",
                "implementation_sha256": "4" * 64,
            },
            "owner": "test",
            "rationale": "Exercise known baseline findings and coverage.",
            "active": blocking_rows,
            "resolved": [],
            "coverage": coverage,
        }
        frozen_encoded, frozen_expected = build_envelope(
            repository, fixture, suite_path, document, frozen
        )
        frozen_run = run(executable, frozen_encoded)
        if frozen_run.returncode:
            raise AssertionError(
                frozen_run.stderr.decode("utf-8", errors="replace")
            )
        frozen_actual = json.loads(frozen_run.stdout)
        frozen_actual.pop("tool")
        frozen_expected.pop("tool")
        if frozen_actual != frozen_expected:
            raise AssertionError("native/oracle frozen baseline result differs")
        if frozen_actual["summary"]["blocking"]:
            raise AssertionError("known baseline findings remained blocking")

        transition = json.loads(json.dumps(frozen))
        transition["rationale"] = "Exercise new, reintroduced, and coverage states."
        transition["active"] = blocking_rows[:1]
        transition["resolved"] = [blocking_rows[1]["fingerprint"]]
        transition["coverage"]["MATRIX-SET-PASS"] = ["A", "B"]
        transition_encoded, transition_expected = build_envelope(
            repository, fixture, suite_path, document, transition
        )
        transition_run = run(executable, transition_encoded)
        if transition_run.returncode:
            raise AssertionError(
                transition_run.stderr.decode("utf-8", errors="replace")
            )
        transition_actual = json.loads(transition_run.stdout)
        transition_actual.pop("tool")
        transition_expected.pop("tool")
        if transition_actual != transition_expected:
            mismatches = [
                key
                for key in sorted(set(transition_actual) | set(transition_expected))
                if transition_actual.get(key) != transition_expected.get(key)
            ]
            raise AssertionError(
                f"native/oracle baseline transition mismatch in {mismatches}"
            )
        transition_data = verify(load_verification_suite(suite_path))
        apply_baseline(transition_data, directory / "input.baseline.json")
        transition_sarif_run = run(executable, transition_encoded, "sarif")
        if transition_sarif_run.returncode:
            raise AssertionError(
                transition_sarif_run.stderr.decode("utf-8", errors="replace")
            )
        transition_native_sarif = json.loads(transition_sarif_run.stdout)
        transition_oracle_sarif = json.loads(
            render_verification_sarif(transition_data)
        )
        transition_native_driver = transition_native_sarif["runs"][0]["tool"][
            "driver"
        ]
        transition_oracle_driver = transition_oracle_sarif["runs"][0]["tool"][
            "driver"
        ]
        transition_oracle_driver["semanticVersion"] = transition_native_driver[
            "semanticVersion"
        ]
        transition_oracle_driver["informationUri"] = transition_native_driver[
            "informationUri"
        ]
        if transition_native_sarif != transition_oracle_sarif:
            raise AssertionError("baseline transition SARIF structure differs")
        transition_junit_run = run(executable, transition_encoded, "junit")
        if transition_junit_run.returncode:
            raise AssertionError(
                transition_junit_run.stderr.decode("utf-8", errors="replace")
            )
        transition_native_junit = ET.fromstring(transition_junit_run.stdout)
        transition_oracle_junit = ET.fromstring(
            render_verification_junit(transition_data)
        )
        transition_native_properties = {
            row.attrib["name"]: row.attrib["value"]
            for row in transition_native_junit.find("properties") or []
        }
        for row in transition_oracle_junit.find("properties") or []:
            if row.attrib["name"] in {
                "archbird.version",
                "archbird.implementation_sha256",
            }:
                row.attrib["value"] = transition_native_properties[
                    row.attrib["name"]
                ]
        if xml_projection(transition_native_junit) != xml_projection(
            transition_oracle_junit
        ):
            raise AssertionError("baseline transition JUnit structure differs")

        variants = []
        expired = json.loads(json.dumps(document))
        expired["policy_date"] = "2027-01-01"
        variants.append(("expired", expired))

        unused = json.loads(json.dumps(document))
        unused["waivers"].append(
            {
                "id": "UNUSED-WAIVER",
                "check": "MATRIX-SET-PASS",
                "comparison": "extra",
                "key": "ABSENT",
                "owner": "test",
                "rationale": "Exercise unused-waiver diagnostics.",
                "expires_on": "2027-12-31",
            }
        )
        variants.append(("unused", unused))

        ambiguous = json.loads(json.dumps(document))
        ambiguous["waivers"].append(
            {
                "id": "DUPLICATE-GROUP-EXTRA",
                "check": "PORT-GROUP-REQUIRED",
                "comparison": "extra",
                "key": "EXTRA",
                "owner": "test",
                "rationale": "Exercise ambiguous-waiver diagnostics.",
                "expires_on": "2027-12-31",
            }
        )
        variants.append(("ambiguous", ambiguous))

        for label, variant in variants:
            variant_path = directory / f"{label}.verify.json"
            variant_path.write_text(
                json.dumps(variant, indent=2, sort_keys=True, ensure_ascii=False)
                + "\n",
                encoding="utf-8",
            )
            variant_encoded, variant_expected = build_envelope(
                repository, fixture, variant_path, variant
            )
            completed = run(executable, variant_encoded)
            if completed.returncode:
                raise AssertionError(
                    f"{label}: "
                    + completed.stderr.decode("utf-8", errors="replace")
                )
            variant_actual = json.loads(completed.stdout)
            variant_actual.pop("tool")
            variant_expected.pop("tool")
            if variant_actual != variant_expected:
                mismatches = [
                    key
                    for key in sorted(set(variant_actual) | set(variant_expected))
                    if variant_actual.get(key) != variant_expected.get(key)
                ]
                raise AssertionError(
                    f"{label}: native/oracle mismatch: "
                    + json.dumps(
                        {
                            key: {
                                "expected": variant_expected.get(key),
                                "actual": variant_actual.get(key),
                            }
                            for key in mismatches
                        },
                        indent=2,
                        ensure_ascii=False,
                    )
                )

        full_path, full_document = full_suite(fixture, directory)
        full_encoded, full_expected = build_envelope(
            repository, fixture, full_path, full_document
        )
        full_run = run(executable, full_encoded)
        if full_run.returncode:
            raise AssertionError(
                "full attestation suite: "
                + full_run.stderr.decode("utf-8", errors="replace")
            )
        full_actual = json.loads(full_run.stdout)
        full_actual.pop("tool")
        full_expected.pop("tool")
        if full_actual != full_expected:
            mismatches = [
                key
                for key in sorted(set(full_actual) | set(full_expected))
                if full_actual.get(key) != full_expected.get(key)
            ]
            details = {
                key: {
                    "expected": full_expected.get(key),
                    "actual": full_actual.get(key),
                }
                for key in mismatches
            }
            raise AssertionError(
                "full attestation native/oracle mismatch: "
                + json.dumps(details, indent=2, ensure_ascii=False)
            )

        full_data = verify(load_verification_suite(full_path))
        sarif_run = run(executable, full_encoded, "sarif")
        if sarif_run.returncode:
            raise AssertionError(sarif_run.stderr.decode("utf-8", errors="replace"))
        native_sarif = json.loads(sarif_run.stdout)
        oracle_sarif = json.loads(render_verification_sarif(full_data))
        native_driver = native_sarif["runs"][0]["tool"]["driver"]
        oracle_driver = oracle_sarif["runs"][0]["tool"]["driver"]
        oracle_driver["semanticVersion"] = native_driver["semanticVersion"]
        oracle_driver["informationUri"] = native_driver["informationUri"]
        if native_sarif != oracle_sarif:
            raise AssertionError("native/oracle SARIF structure differs")

        junit_run = run(executable, full_encoded, "junit")
        if junit_run.returncode:
            raise AssertionError(junit_run.stderr.decode("utf-8", errors="replace"))
        native_junit = ET.fromstring(junit_run.stdout)
        oracle_junit = ET.fromstring(render_verification_junit(full_data))
        native_properties = {
            row.attrib["name"]: row.attrib["value"]
            for row in native_junit.find("properties") or []
        }
        for row in oracle_junit.find("properties") or []:
            if row.attrib["name"] in {
                "archbird.version",
                "archbird.implementation_sha256",
            }:
                row.attrib["value"] = native_properties[row.attrib["name"]]
        if xml_projection(native_junit) != xml_projection(oracle_junit):
            raise AssertionError("native/oracle JUnit structure differs")

        markdown_run = run(executable, full_encoded, "markdown")
        if markdown_run.returncode:
            raise AssertionError(
                markdown_run.stderr.decode("utf-8", errors="replace")
            )
        native_markdown = markdown_run.stdout.decode("utf-8")
        oracle_markdown = render_verification_markdown(full_data)
        identity_pattern = r"Evidence: Archbird [^ ]+ `[0-9a-f]{16}`;"
        native_markdown = re.sub(
            identity_pattern, "Evidence: Archbird <native> `<digest>`;", native_markdown
        )
        oracle_markdown = re.sub(
            identity_pattern, "Evidence: Archbird <native> `<digest>`;", oracle_markdown
        )
        oracle_markdown = native_markdown_projection(oracle_markdown)
        if native_markdown != oracle_markdown:
            raise AssertionError(
                "native/oracle Markdown projection differs\n"
                + "".join(
                    difflib.unified_diff(
                        oracle_markdown.splitlines(keepends=True),
                        native_markdown.splitlines(keepends=True),
                        fromfile="oracle",
                        tofile="native",
                    )
                )
            )
        compact_run = run(executable, full_encoded, "markdown", "3")
        if compact_run.returncode:
            raise AssertionError(
                compact_run.stderr.decode("utf-8", errors="replace")
            )
        native_compact = re.sub(
            identity_pattern,
            "Evidence: Archbird <native> `<digest>`;",
            compact_run.stdout.decode("utf-8"),
        )
        oracle_compact = re.sub(
            identity_pattern,
            "Evidence: Archbird <native> `<digest>`;",
            render_verification_markdown(full_data, max_findings=3),
        )
        oracle_compact = native_markdown_projection(oracle_compact)
        if native_compact != oracle_compact:
            raise AssertionError("native/oracle compact Markdown projection differs")

        def invalid_cases(documents: dict) -> None:
            documents["subject.behavior"]["cases"] = []

        def stale_evidence(documents: dict) -> None:
            subject = documents["subject.behavior"]
            subject["producer"]["evidence"][0]["sha256"] = "0" * 64
            subject["producer"]["evidence_slice_sha256"] = evidence_slice(subject)

        def exact_bits(documents: dict) -> None:
            for document in documents.values():
                case = next(row for row in document["cases"] if row["id"] == "reduce.float")
                case["comparison"] = {"kind": "exact_bits"}

        def inapplicable_reference(documents: dict) -> None:
            for document in documents.values():
                case = next(row for row in document["cases"] if row["id"] == "permute.valid")
                case["requires"] = ["shape", "gpu"]

        for label, mutate, expected_state in (
            ("attestation-invalid", invalid_cases, "unknown"),
            ("attestation-stale", stale_evidence, "stale"),
            ("attestation-exact-bits", exact_bits, "current"),
            ("attestation-inapplicable", inapplicable_reference, "current"),
        ):
            variant_path, variant_document = attestation_variant_suite(
                fixture, directory, label, mutate
            )
            variant_encoded, variant_expected = build_envelope(
                repository, fixture, variant_path, variant_document
            )
            completed = run(executable, variant_encoded)
            if completed.returncode:
                raise AssertionError(
                    f"{label}: "
                    + completed.stderr.decode("utf-8", errors="replace")
                )
            variant_actual = json.loads(completed.stdout)
            variant_actual.pop("tool")
            variant_expected.pop("tool")
            if variant_actual != variant_expected:
                mismatches = [
                    key
                    for key in sorted(set(variant_actual) | set(variant_expected))
                    if variant_actual.get(key) != variant_expected.get(key)
                ]
                raise AssertionError(
                    f"{label}: native/oracle mismatch: "
                    + json.dumps(
                        {
                            key: {
                                "expected": variant_expected.get(key),
                                "actual": variant_actual.get(key),
                            }
                            for key in mismatches
                        },
                        indent=2,
                        ensure_ascii=False,
                    )
                )
            subject_state = next(
                row["state"]
                for row in variant_actual["attestations"]
                if row["name"] == "subject.behavior"
            )
            if label != "attestation-inapplicable" and subject_state != expected_state:
                raise AssertionError(
                    f"{label}: subject state={subject_state}, expected={expected_state}"
                )

        candidate = json.loads(encoded)
        candidate["suite"]["candidate"] = True
        rejected = run(executable, json.dumps(candidate).encode("utf-8"))
        if rejected.returncode == 0 or b"candidate" not in rejected.stderr:
            raise AssertionError("native core accepted an unreviewed candidate suite")

    print(
        "static verification results match oracle: "
        f"checks={len(actual['checks'])} "
        f"findings={actual['summary']['findings']['total']} "
        f"facts={len(actual['facts'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
