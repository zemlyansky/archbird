#!/usr/bin/env python3
"""Differential verification-suite validation and deterministic host plan."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import sys

from archbird.verify.config import load_verification_suite


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load extension {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def mutations(base: dict) -> list[tuple[str, dict]]:
    rows: list[tuple[str, dict]] = [("base", copy.deepcopy(base))]

    def add(name: str, edit) -> None:
        value = copy.deepcopy(base)
        edit(value)
        rows.append((name, value))

    add("root-unknown", lambda value: value.__setitem__("surprise", 1))
    add("schema", lambda value: value.__setitem__("schema_version", 2))
    add("candidate-type", lambda value: value.__setitem__("candidate", "yes"))
    add("policy-date", lambda value: value.__setitem__("policy_date", "2026-02-30"))
    add("empty-projects", lambda value: value.__setitem__("projects", {}))
    add(
        "project-both",
        lambda value: value["projects"]["subject"].__setitem__("map", "map.json"),
    )
    add(
        "project-unknown",
        lambda value: value["projects"]["subject"].__setitem__("x", 1),
    )
    add(
        "project-absolute",
        lambda value: value["projects"]["subject"].__setitem__("root", "/x"),
    )
    add(
        "capability-duplicate",
        lambda value: value["projects"]["subject"].__setitem__(
            "capabilities", ["shape", "shape"]
        ),
    )
    add(
        "parameter-container",
        lambda value: value["projects"]["subject"].__setitem__(
            "parameters", {"rank": []}
        ),
    )
    add(
        "extractor-kind",
        lambda value: value["extractors"]["subject.ops"].__setitem__(
            "kind", "magic"
        ),
    )
    add(
        "extractor-kind-field",
        lambda value: value["extractors"]["subject.ops"].__setitem__(
            "class", "Wrong"
        ),
    )
    add(
        "extractor-project",
        lambda value: value["extractors"]["subject.ops"].__setitem__(
            "project", "missing"
        ),
    )
    add(
        "extractor-parent-path",
        lambda value: value["extractors"]["subject.ops"].__setitem__(
            "path", "../core.h"
        ),
    )
    add(
        "macro-same-argument",
        lambda value: value["extractors"]["subject.required"].update(
            {"selector_argument": 1, "values_from_argument": 1}
        ),
    )
    add(
        "literal-duplicate",
        lambda value: value["extractors"]["relation.allowed"].__setitem__(
            "rows",
            [
                {"source": "a", "kind": "x", "target": "b"},
                {"source": "a", "kind": "x", "target": "b"},
            ],
        ),
    )
    add(
        "mapping-target-duplicate",
        lambda value: value["mappings"]["ops"].__setitem__(
            "actual_to_expected", {"A": "X", "B": "X"}
        ),
    )
    add(
        "attestation-project",
        lambda value: value["attestations"]["subject.behavior"].__setitem__(
            "project", "missing"
        ),
    )
    add("empty-checks", lambda value: value.__setitem__("checks", []))
    add("check-unknown", lambda value: value["checks"][0].__setitem__("x", 1))
    add(
        "check-kind-field",
        lambda value: value["checks"][0].__setitem__("min", 1),
    )
    add("check-missing", lambda value: value["checks"][0].pop("actual"))
    add(
        "check-operand",
        lambda value: value["checks"][0].__setitem__("actual", "missing.fact"),
    )
    add(
        "check-severity",
        lambda value: value["checks"][0].__setitem__("severity", "critical"),
    )
    add(
        "cardinality-empty",
        lambda value: [
            row.pop("exact")
            for row in value["checks"]
            if row["id"] == "REFERENCE-OPS-CARDINALITY"
        ],
    )
    add(
        "waiver-no-bound",
        lambda value: value["waivers"][0].pop("expires_on"),
    )
    add(
        "waiver-mixed-key",
        lambda value: value["waivers"][0].__setitem__("fingerprint", "0" * 64),
    )
    add(
        "waiver-unknown-check",
        lambda value: value["waivers"][0].__setitem__("check", "MISSING"),
    )
    add(
        "waiver-bad-date",
        lambda value: value["waivers"][0].__setitem__("expires_on", "2026-13-01"),
    )
    add(
        "valid-wildcard",
        lambda value: value["extractors"]["relation.allowed"].__setitem__(
            "rows", [{"source": "*", "target": "*"}]
        ),
    )
    return rows


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_verify_config.py EXTENSION REPO FIXTURE")
    native = load_extension(Path(sys.argv[1]).resolve())
    repository = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    base = json.loads(fixture.read_text(encoding="utf-8"))
    plan = json.loads(native.verification_plan(fixture.read_bytes()))
    if plan["suite"]["sha256"] != load_verification_suite(fixture).digest:
        raise AssertionError("native/oracle verification suite digest differs")
    if [row["name"] for row in plan["projects"]] != ["reference", "subject"]:
        raise AssertionError(plan)
    if {row["name"]: row["provider_scan"] for row in plan["projects"]} != {
        "reference": False,
        "subject": True,
    }:
        raise AssertionError(f"unexpected verification evidence plan: {plan!r}")
    if {row["provider"] for row in plan["sources"]} != {
        "native-c",
        "python-ast",
    }:
        raise AssertionError(plan)

    mismatches: list[str] = []
    with tempfile.TemporaryDirectory(dir=repository / "build") as directory:
        path = Path(directory) / "suite.json"
        for name, document in mutations(base):
            encoded = json.dumps(
                document,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=False,
                allow_nan=False,
            ).encode("utf-8")
            path.write_bytes(encoded)
            try:
                load_verification_suite(path)
                oracle = True
            except Exception:
                oracle = False
            try:
                native.verification_plan(encoded)
                port = True
            except Exception:
                port = False
            if oracle != port:
                mismatches.append(f"{name}: oracle={oracle} native={port}")
    if mismatches:
        raise AssertionError("suite validation mismatches:\n" + "\n".join(mismatches))
    print(f"verification suite validation matches oracle: {len(mutations(base))} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
