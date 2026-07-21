#!/usr/bin/env python3
"""Component-membership extraction, policy, and debug contracts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load extension {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def encoded(document: object) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode()


def map_document() -> dict:
    paths = ("src/a.c", "src/b.c", "src/c.c", "src/d.c")
    return {
        "artifact": "map",
        "schema_version": 7,
        "project": "membership-test",
        "evidence": {"config_sha256": "1" * 64, "input_sha256": "2" * 64},
        "tool": {
            "name": "archbird",
            "version": "fixture",
            "implementation_sha256": "3" * 64,
        },
        "diagnostics": [],
        "files": [
            {
                "bytes": index,
                "language": "c",
                "layer": "core" if index < 3 else "support",
                "path": path,
                "sha256": str(index + 3) * 64,
            }
            for index, path in enumerate(paths, start=1)
        ],
        "components": [
            {
                "description": "",
                "files": ["src/a.c", "src/b.c"],
                "name": "core",
                "outgoing": {},
                "symbol_count": 0,
            },
            {
                "description": "",
                "files": ["src/b.c", "src/c.c"],
                "name": "ui",
                "outgoing": {},
                "symbol_count": 0,
            },
            {
                "description": "",
                "files": [],
                "name": "empty",
                "outgoing": {},
                "symbol_count": 0,
            },
        ],
    }


def suite_document(*, multiple_projects: bool = False) -> dict:
    projects = {"subject": {"map": "ARCHBIRD.json"}}
    if multiple_projects:
        projects["reference"] = {"map": "REFERENCE.json"}
    return {
        "schema_version": 1,
        "suite": "membership-suite",
        "projects": projects,
        "extractors": {
            "architecture.membership": {
                "kind": "component_membership",
                "project": "subject",
            }
        },
        "checks": [
            {
                "id": "ASSIGNED",
                "assert": "numeric_bounds",
                "actual": "architecture.membership",
                "min": 1,
                "owner": "test",
                "rationale": "Every mapped source belongs to a component.",
            },
            {
                "id": "EXCLUSIVE",
                "assert": "numeric_bounds",
                "actual": "architecture.membership",
                "max": 1,
                "owner": "test",
                "rationale": "This fixture requires exclusive membership.",
            },
            {
                "id": "EXACT",
                "assert": "numeric_bounds",
                "actual": "architecture.membership",
                "exact": 1,
                "owner": "test",
                "rationale": "Exercise combined membership bounds.",
            },
        ],
    }


def input_document(map_value: dict, *, multiple_projects: bool = False) -> dict:
    projects = [{"name": "subject", "map": map_value, "sources": []}]
    if multiple_projects:
        reference = json.loads(json.dumps(map_value))
        reference["project"] = "membership-reference"
        projects.append({"name": "reference", "map": reference, "sources": []})
    return {
        "artifact": "verification-input",
        "schema_version": 1,
        "suite_path": "archbird.verify.json",
        "projects": projects,
        "provided_facts": [],
        "attestations": [],
        "baseline": None,
    }


def request(
    view: str,
    *,
    project: str | None = None,
    component: str | None = None,
    limit: int | None = None,
) -> bytes:
    result = {
        "artifact": "verification-debug-request",
        "schema_version": 1,
        "view": view,
    }
    if project is not None:
        result["project"] = project
    if component is not None:
        result["component"] = component
    if limit is not None:
        result["limit"] = limit
    return encoded(result)


def analyze(extension, suite: dict, input_value: dict) -> dict:
    return json.loads(extension.verification_analyze(encoded(suite), encoded(input_value)))


def debug(extension, suite: dict, input_value: dict, request_json: bytes) -> dict:
    return json.loads(
        extension.verification_debug(
            encoded(suite), encoded(input_value), request_json, "json", False
        )
    )


def assert_rejected(call, message: str) -> None:
    try:
        call()
    except Exception as error:
        if message not in str(error):
            raise
    else:
        raise AssertionError(f"expected rejection containing {message!r}")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_verify_membership.py EXTENSION REPOSITORY")
    extension = load_extension(Path(sys.argv[1]).resolve())
    repository = Path(sys.argv[2]).resolve()
    suite = suite_document()
    map_value = map_document()
    input_value = input_document(map_value)

    result = analyze(extension, suite, input_value)
    fact = result["facts"][0]
    items = {row["key"]: row for row in fact["items"]}
    if (
        fact["name"] != "architecture.membership"
        or fact["shape"] != "values"
        or {key: row["value"] for key, row in items.items()}
        != {"src/a.c": 1, "src/b.c": 2, "src/c.c": 1, "src/d.c": 0}
        or items["src/b.c"]["attributes"]["components"] != ["core", "ui"]
        or items["src/b.c"]["attributes"]["membership"] != "overlap"
        or items["src/d.c"]["attributes"]["membership"] != "unassigned"
    ):
        raise AssertionError(f"membership fact is incomplete: {fact!r}")
    checks = {row["id"]: row for row in result["checks"]}
    if (
        [row["key"] for row in checks["ASSIGNED"]["findings"]] != ["src/d.c"]
        or [row["key"] for row in checks["EXCLUSIVE"]["findings"]]
        != ["src/b.c"]
        or [row["key"] for row in checks["EXACT"]["findings"]]
        != ["src/b.c", "src/d.c"]
    ):
        raise AssertionError(f"membership bounds drifted: {checks!r}")

    component = debug(extension, suite, input_value, request("component", component="core"))
    component_row = component["memberships"][0]
    if (
        component_row["component"]
        != {"exclusive_files": 1, "files": 2, "name": "core", "overlapping_files": 1}
        or [row["path"] for row in component_row["files"]]
        != ["src/a.c", "src/b.c"]
    ):
        raise AssertionError(f"component view is incomplete: {component_row!r}")
    bounded_component = debug(
        extension,
        suite,
        input_value,
        request("component", component="core", limit=1),
    )["memberships"][0]
    if (
        [row["path"] for row in bounded_component["files"]] != ["src/a.c"]
        or bounded_component["selection"]
        != {"matched": 2, "rendered": 1, "truncated": True}
    ):
        raise AssertionError("membership witness limiting is not explicit")
    empty_component = debug(
        extension, suite, input_value, request("component", component="empty")
    )["memberships"][0]
    if empty_component["component"]["files"] != 0 or empty_component["files"]:
        raise AssertionError("empty component was not reported explicitly")
    unassigned = debug(extension, suite, input_value, request("unassigned"))
    overlap = debug(extension, suite, input_value, request("overlap"))
    counts = unassigned["memberships"][0]["counts"]
    if (
        counts
        != {
            "assigned": 3,
            "assignments": 4,
            "components": 3,
            "empty_components": 1,
            "mapped_files": 4,
            "overlapping": 1,
            "unassigned": 1,
        }
        or [row["path"] for row in unassigned["memberships"][0]["files"]]
        != ["src/d.c"]
        or [row["path"] for row in overlap["memberships"][0]["files"]]
        != ["src/b.c"]
        or unassigned["memberships"][0]["selection"]
        != {"matched": 1, "rendered": 1, "truncated": False}
        or unassigned["selections"]
        or unassigned["checks"]
    ):
        raise AssertionError("membership debug accounting drifted")
    markdown = extension.verification_debug(
        encoded(suite),
        encoded(input_value),
        request("unassigned"),
        "markdown",
        False,
    )
    if b"mapped_files=4" not in markdown or b"components=(none)" not in markdown:
        raise AssertionError(markdown.decode(errors="replace"))

    assert_rejected(
        lambda: debug(
            extension,
            suite,
            input_value,
            request("component", component="missing"),
        ),
        "component is unknown",
    )
    invalid_filter = json.loads(request("unassigned"))
    invalid_filter["component"] = "core"
    assert_rejected(
        lambda: debug(extension, suite, input_value, encoded(invalid_filter)),
        "component filter requires component view",
    )

    missing_components = json.loads(json.dumps(map_value))
    del missing_components["components"]
    unknown = analyze(extension, suite, input_document(missing_components))
    if unknown["facts"][0]["state"] != "unknown" or unknown["checks"][0]["status"] != "unknown":
        raise AssertionError("missing component inventory passed silently")
    unknown_debug = debug(
        extension, suite, input_document(missing_components), request("unassigned")
    )["memberships"][0]
    if unknown_debug["state"] != "unknown" or unknown_debug["counts"]["mapped_files"] is not None:
        raise AssertionError("unknown membership inventory was rendered as exact")

    dangling = json.loads(json.dumps(map_value))
    dangling["components"][0]["files"].append("src/missing.c")
    dangling_result = analyze(extension, suite, input_document(dangling))
    if dangling_result["facts"][0]["state"] != "unknown":
        raise AssertionError("dangling component assignment passed silently")

    stale_suite = json.loads(json.dumps(suite))
    stale_suite["projects"]["subject"]["source_lock"] = {
        "src/a.c": "0" * 64
    }
    stale_map = json.loads(json.dumps(map_value))
    source_text = "current source\n"
    stale_map["files"][0]["sha256"] = hashlib.sha256(
        source_text.encode()
    ).hexdigest()
    stale_input = input_document(stale_map)
    stale_input["projects"][0]["sources"] = [
        {"path": "src/a.c", "text": source_text}
    ]
    stale_result = analyze(extension, stale_suite, stale_input)
    stale_membership = debug(
        extension, stale_suite, stale_input, request("unassigned")
    )["memberships"][0]
    if (
        stale_result["facts"][0]["state"] != "stale"
        or stale_membership["state"] != "stale"
        or "does not match" not in stale_membership["message"]
    ):
        raise AssertionError("membership debug ignored a stale project source lock")

    multi_suite = suite_document(multiple_projects=True)
    multi_input = input_document(map_value, multiple_projects=True)
    assert_rejected(
        lambda: debug(
            extension,
            multi_suite,
            multi_input,
            request("component", component="core"),
        ),
        "requires a project",
    )
    selected_project = debug(
        extension,
        multi_suite,
        multi_input,
        request("component", project="subject", component="core"),
    )
    if [row["project"] for row in selected_project["memberships"]] != ["subject"]:
        raise AssertionError("exact membership project filter was ignored")

    with tempfile.TemporaryDirectory(dir=repository / "build") as raw_directory:
        root = Path(raw_directory)
        (root / "archbird.verify.json").write_bytes(encoded(suite))
        (root / "ARCHBIRD.json").write_bytes(encoded(map_value))
        for arguments, expected_path in (
            (("component", "core"), "src/a.c"),
            (("unassigned",), "src/d.c"),
            (("overlap",), "src/b.c"),
        ):
            completed = subprocess.run(
                [
                    str(repository / "archbird"),
                    "verify",
                    "debug",
                    *arguments,
                    str(root),
                    "--format",
                    "json",
                ],
                capture_output=True,
                check=False,
            )
            if completed.returncode:
                raise AssertionError(completed.stderr.decode(errors="replace"))
            cli_result = json.loads(completed.stdout)
            if expected_path not in {
                row["path"] for row in cli_result["memberships"][0]["files"]
            }:
                raise AssertionError(f"CLI membership view drifted: {cli_result!r}")

    print("verification membership contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
