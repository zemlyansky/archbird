#!/usr/bin/env python3
"""Differential native fact IR over source, map, literal, and host providers."""

from __future__ import annotations

import importlib.util
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from archbird.map.render import render_json
from archbird.verify.config import load_verification_suite
from archbird.verify.engine import verify
from archbird.verify.render import verification_dict


def load_provider(path: Path):
    spec = importlib.util.spec_from_file_location(
        "archbird_native_verification_provider", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load provider {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def envelope(repository: Path, fixture: Path) -> tuple[bytes, list[dict]]:
    suite_path = fixture / "verification.json"
    suite_document = json.loads(suite_path.read_text(encoding="utf-8"))
    suite = load_verification_suite(suite_path)
    data = verify(suite)
    provider = load_provider(
        repository / "py/archbird/providers/verification.py"
    )
    projects = []
    provided = []
    for name, project in data.projects.items():
        root = fixture / name
        paths = sorted(
            {
                row["path"]
                for row in suite_document["extractors"].values()
                if row.get("project") == name and "path" in row
            }
        )
        sources = [
            {"path": path, "text": (root / path).read_text(encoding="utf-8")}
            for path in paths
        ]
        projects.append(
            {
                "name": name,
                "map": json.loads(render_json(project.data)),
                "sources": sources,
            }
        )
        for fact_name, row in sorted(suite_document["extractors"].items()):
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
    attestations = [
        {
            "name": name,
            "path": row["path"],
            "document": json.loads(
                (fixture / row["path"]).read_text(encoding="utf-8")
            ),
        }
        for name, row in sorted(suite_document["attestations"].items())
    ]
    document = {
        "suite": suite_document,
        "input": {
            "schema_version": 1,
            "artifact": "verification-input",
            "suite_path": "verification.json",
            "projects": projects,
            "provided_facts": provided,
            "attestations": attestations,
            "baseline": None,
        },
    }
    expected = list(verification_dict(data)["facts"])
    return json.dumps(
        document,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8"), expected


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_verify_facts.py CLI REPO FIXTURE")
    executable = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    fixture = Path(sys.argv[3]).resolve()
    encoded, expected = envelope(repository, fixture)
    completed = subprocess.run(
        [str(executable)], input=encoded, capture_output=True, check=False
    )
    if completed.returncode:
        raise AssertionError(completed.stderr.decode("utf-8", errors="replace"))
    actual = json.loads(completed.stdout)
    if actual != expected:
        expected_by_name = {row["name"]: row for row in expected}
        actual_by_name = {row["name"]: row for row in actual}
        mismatches = [
            name
            for name in sorted(set(expected_by_name) | set(actual_by_name))
            if expected_by_name.get(name) != actual_by_name.get(name)
        ]
        raise AssertionError(f"native/oracle fact mismatch: {mismatches}")

    map_fixture = repository / "test/fixtures/map_packages"
    with tempfile.TemporaryDirectory(dir=repository / "build") as directory:
        suite_path = Path(directory) / "map.verify.json"
        relative_config = Path(
            __import__("os").path.relpath(
                map_fixture / "archbird.json", suite_path.parent
            )
        ).as_posix()
        map_suite = {
            "schema_version": 1,
            "suite": "map-extractors",
            "projects": {"subject": {"config": relative_config}},
            "extractors": {
                "map.components": {
                    "kind": "component_edges",
                    "project": "subject",
                },
                "map.files": {
                    "kind": "file_edges",
                    "project": "subject",
                    "kinds": ["call", "declaration", "import"],
                },
                "map.surface": {
                    "kind": "provider_surface",
                    "project": "subject",
                    "name": "python-c",
                },
                "map.symbols": {
                    "kind": "symbols",
                    "project": "subject",
                    "paths": ["src"],
                },
                "map.tests": {
                    "kind": "test_routes",
                    "project": "subject",
                    "group": "c",
                },
            },
            "checks": [
                {
                    "id": "MAP-SYMBOLS",
                    "assert": "cardinality",
                    "actual": "map.symbols",
                    "min": 0,
                    "owner": "test",
                    "rationale": "Exercise all map-derived fact providers.",
                }
            ],
        }
        suite_path.write_text(json.dumps(map_suite), encoding="utf-8")
        suite = load_verification_suite(suite_path)
        map_data = verify(suite)
        project = map_data.projects["subject"]
        evidence_paths = set(project.data.files)
        for surface in project.data.surfaces.values():
            for row in surface.names:
                evidence_paths.update(item.path for item in row.declarations)
                evidence_paths.update(item.path for item in row.uses)
                evidence_paths.update(row.candidates)
        sources = [
            {
                "path": path,
                "text": (map_fixture / path).read_text(encoding="utf-8"),
            }
            for path in sorted(evidence_paths)
        ]
        map_input = {
            "suite": map_suite,
            "input": {
                "schema_version": 1,
                "artifact": "verification-input",
                "suite_path": "map.verify.json",
                "projects": [
                    {
                        "name": "subject",
                        "map": json.loads(render_json(project.data)),
                        "sources": sources,
                    }
                ],
                "provided_facts": [],
                "attestations": [],
                "baseline": None,
            },
        }
        mapped = subprocess.run(
            [str(executable)],
            input=json.dumps(map_input).encode("utf-8"),
            capture_output=True,
            check=False,
        )
        if mapped.returncode:
            raise AssertionError(mapped.stderr.decode("utf-8", errors="replace"))
        expected_map_facts = list(verification_dict(map_data)["facts"])
        actual_map_facts = json.loads(mapped.stdout)
        if actual_map_facts != expected_map_facts:
            expected_by_name = {row["name"]: row for row in expected_map_facts}
            actual_by_name = {row["name"]: row for row in actual_map_facts}
            mismatches = [
                name
                for name in sorted(set(expected_by_name) | set(actual_by_name))
                if expected_by_name.get(name) != actual_by_name.get(name)
            ]
            details = {
                name: {
                    "expected": expected_by_name.get(name),
                    "actual": actual_by_name.get(name),
                }
                for name in mismatches
            }
            raise AssertionError(
                "native/oracle map fact mismatch: "
                + json.dumps(details, indent=2, ensure_ascii=False)
            )

        evidence_input = copy.deepcopy(map_input)
        evidence_map = evidence_input["input"]["projects"][0]["map"]
        evidence_edge = next(
            row
            for row in evidence_map["edges"]
            if row["kind"] in {"call", "declaration", "import"}
        )
        evidence_edge["evidence"] = [
            {
                "basis": "semantic-index",
                "provider": "compiler",
                "state": "unknown",
            }
        ]
        evidence_run = subprocess.run(
            [str(executable)],
            input=json.dumps(evidence_input).encode("utf-8"),
            capture_output=True,
            check=False,
        )
        if evidence_run.returncode:
            raise AssertionError(
                evidence_run.stderr.decode("utf-8", errors="replace")
            )
        evidence_facts = {
            row["name"]: row for row in json.loads(evidence_run.stdout)
        }
        edge_item = next(
            item
            for item in evidence_facts["map.files"]["items"]
            if item["attributes"]
            == {
                "kind": evidence_edge["kind"],
                "source": evidence_edge["source"],
                "target": evidence_edge["target"],
            }
        )
        if edge_item["state"] != "unknown" or not any(
            "semantic-index:compiler:unknown" in row["detail"]
            for row in edge_item["evidence"]
        ):
            raise AssertionError(
                f"unknown semantic-edge freshness was lost: {edge_item!r}"
            )

        evidence_edge["evidence"] = [
            {
                "basis": "semantic-index",
                "provider": "compiler-current",
                "state": "current",
            },
            {
                "basis": "semantic-index",
                "provider": "compiler-unknown",
                "state": "unknown",
            },
        ]
        mixed_run = subprocess.run(
            [str(executable)],
            input=json.dumps(evidence_input).encode("utf-8"),
            capture_output=True,
            check=False,
        )
        if mixed_run.returncode:
            raise AssertionError(
                mixed_run.stderr.decode("utf-8", errors="replace")
            )
        mixed_facts = {row["name"]: row for row in json.loads(mixed_run.stdout)}
        mixed_item = next(
            item
            for item in mixed_facts["map.files"]["items"]
            if item["attributes"]
            == {
                "kind": evidence_edge["kind"],
                "source": evidence_edge["source"],
                "target": evidence_edge["target"],
            }
        )
        if mixed_item["state"] != "current":
            raise AssertionError(
                f"current semantic evidence did not dominate unknown: {mixed_item!r}"
            )

    tampered = json.loads(encoded)
    tampered["input"]["provided_facts"][0]["items"][0]["evidence"][0][
        "sha256"
    ] = "0" * 64
    rejected = subprocess.run(
        [str(executable)],
        input=json.dumps(tampered).encode("utf-8"),
        capture_output=True,
        check=False,
    )
    if rejected.returncode == 0 or b"stale" not in rejected.stderr:
        raise AssertionError("native core accepted stale precision-provider evidence")
    print(
        "verification facts match oracle: "
        f"source_facts={len(actual)} map_facts={len(actual_map_facts)} "
        f"items={sum(len(row['items']) for row in actual + actual_map_facts)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
