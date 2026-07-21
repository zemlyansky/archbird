#!/usr/bin/env python3
"""Native verification recipe contracts and config-free CLI behavior."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from archbird.native import Verification


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load extension {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def encoded(document: object) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode()


def request(*, maximum: int = 10, include: list[str] | None = None) -> dict:
    return {
        "artifact": "verification-recipe-request",
        "schema_version": 1,
        "recipe": "max-file-bytes",
        "project": {"map": "ARCHBIRD.json"},
        "arguments": {"max": maximum, "include": include or ["src/**"]},
        "check": {
            "id": "MAX-FILE-BYTES",
            "owner": "test",
            "rationale": "Exercise portable verification recipes.",
        },
    }


def map_document() -> dict:
    return {
        "artifact": "map",
        "schema_version": 7,
        "project": "recipe-test",
        "evidence": {"config_sha256": "1" * 64, "input_sha256": "2" * 64},
        "tool": {
            "name": "archbird",
            "version": "fixture",
            "implementation_sha256": "3" * 64,
        },
        "diagnostics": [],
        "discovery": {"coverage": {"oversized": 1}, "sha256": "5" * 64},
        "files": [
            {
                "bytes": 5,
                "language": "c",
                "layer": "core",
                "path": "src/small.c",
                "sha256": "4" * 64,
            }
        ],
    }


def resolution_document() -> dict:
    return {
        "artifact": "archbird-config-resolution",
        "schema_version": 1,
        "configuration_sha256": "1" * 64,
        "project": "recipe-test",
        "sha256": "5" * 64,
        "coverage": {"oversized": 1},
        "diagnostics": [
            {
                "bytes": 20,
                "code": "discovery-file-oversized",
                "limit": 10,
                "path": "src/large.c",
                "severity": "warning",
            }
        ],
    }


def verification_input(*, resolution: dict | None) -> bytes:
    project = {"name": "subject", "map": map_document(), "sources": []}
    if resolution is not None:
        project["resolution"] = resolution
    return encoded(
        {
            "artifact": "verification-input",
            "schema_version": 1,
            "suite_path": "recipe.verify.json",
            "projects": [project],
            "provided_facts": [],
            "attestations": [],
            "baseline": None,
        }
    )


def analyze(extension, suite: bytes, input_json: bytes) -> dict:
    return json.loads(extension.verification_analyze(suite, input_json))


def run_cli(repository: Path, *arguments: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(repository / "archbird"), "verify", "recipe", *arguments],
        cwd=repository,
        capture_output=True,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_verify_recipes.py EXTENSION REPOSITORY")
    extension = load_extension(Path(sys.argv[1]).resolve())
    repository = Path(sys.argv[2]).resolve()

    catalog = json.loads(extension.verification_recipe_catalog("", False))
    if [row["name"] for row in catalog["recipes"]] != ["max-file-bytes"]:
        raise AssertionError(f"unexpected recipe catalog: {catalog!r}")
    shown = json.loads(
        extension.verification_recipe_catalog("max-file-bytes", False)
    )
    if shown != catalog or not shown["recipes"][0]["nonempty_by_default"]:
        raise AssertionError("recipe show and list contracts differ")

    suite = extension.verification_recipe_compile(encoded(request()), False)
    suite_document = json.loads(suite)
    plan = json.loads(extension.verification_plan(suite, False))
    if (
        suite_document["extractors"]["recipe.file-bytes"]["kind"]
        != "file_metrics"
        or suite_document["checks"][0]["assert"] != "numeric_bounds"
        or plan["projects"][0]["provider_scan"]
    ):
        raise AssertionError("recipe did not compile to inventory-only primitives")
    invalid = request()
    invalid["arguments"]["surprise"] = True
    try:
        extension.verification_recipe_compile(encoded(invalid), False)
    except Exception as error:
        if "unknown field" not in str(error):
            raise
    else:
        raise AssertionError("recipe compiler accepted an unknown argument")

    missing_resolution = analyze(extension, suite, verification_input(resolution=None))
    if (
        missing_resolution["checks"][0]["status"] != "unknown"
        or "incomplete" not in missing_resolution["checks"][0]["findings"][0]["message"]
    ):
        raise AssertionError("saved Map hid omitted oversized files")

    missing_coverage_input = json.loads(verification_input(resolution=None))
    del missing_coverage_input["projects"][0]["map"]["discovery"]
    missing_coverage = analyze(extension, suite, encoded(missing_coverage_input))
    if (
        missing_coverage["checks"][0]["status"] != "unknown"
        or "coverage" not in missing_coverage["checks"][0]["findings"][0]["message"]
    ):
        raise AssertionError("saved Map without omission coverage passed silently")

    input_json = verification_input(resolution=resolution_document())
    failed = analyze(extension, suite, input_json)
    check = failed["checks"][0]
    if (
        check["status"] != "fail"
        or check["coverage"] != ["src/small.c"]
        or [row["key"] for row in check["findings"]] != ["src/large.c"]
        or check["findings"][0]["evidence"][0]["detail"]
        != "discovery-file-oversized bytes=20"
    ):
        raise AssertionError(f"oversized file evidence is incomplete: {check!r}")

    passing_suite = extension.verification_recipe_compile(
        encoded(request(maximum=20)), False
    )
    passed = analyze(extension, passing_suite, input_json)
    if passed["checks"][0]["status"] != "pass" or passed["checks"][0][
        "coverage"
    ] != ["src/large.c", "src/small.c"]:
        raise AssertionError("numeric inclusive maximum or coverage changed")

    empty_request = request(include=["missing/**"])
    empty_suite = extension.verification_recipe_compile(encoded(empty_request), False)
    empty = analyze(extension, empty_suite, input_json)
    if empty["checks"][0]["findings"][0]["key"] != "selection:empty":
        raise AssertionError("empty selection passed silently")
    empty_request["arguments"]["allow_empty"] = True
    allowed_suite = extension.verification_recipe_compile(
        encoded(empty_request), False
    )
    if analyze(extension, allowed_suite, input_json)["checks"][0]["status"] != "pass":
        raise AssertionError("explicit allow_empty was ignored")

    for format_name in ("markdown", "sarif", "junit"):
        report = extension.verification_report(
            suite, input_json, format_name, 200, format_name == "sarif"
        )
        if format_name == "markdown" and b"src/large.c" not in report:
            raise AssertionError("Markdown report omitted the failing path")
        if format_name == "sarif" and json.loads(report)["version"] != "2.1.0":
            raise AssertionError("invalid recipe SARIF report")
        if format_name == "junit":
            ET.fromstring(report)

    mismatched = resolution_document()
    mismatched["coverage"]["oversized"] = 2
    try:
        analyze(extension, suite, verification_input(resolution=mismatched))
    except Exception as error:
        if "oversized" not in str(error):
            raise
    else:
        raise AssertionError("verification accepted incomplete resolution evidence")

    mismatched = resolution_document()
    mismatched["sha256"] = "6" * 64
    try:
        analyze(extension, suite, verification_input(resolution=mismatched))
    except Exception as error:
        if "digest" not in str(error):
            raise
    else:
        raise AssertionError("verification accepted a resolution for another Map")

    with tempfile.TemporaryDirectory(dir=repository / "build") as raw_directory:
        root = Path(raw_directory)
        (root / "small.py").write_text("def answer():\n    return 42\n", encoding="utf-8")
        listed = run_cli(repository, "list")
        if listed.returncode or b"max-file-bytes" not in listed.stdout:
            raise AssertionError(listed.stderr.decode(errors="replace"))
        failed_cli = run_cli(
            repository,
            "run",
            "max-file-bytes",
            str(root),
            "--max",
            "1B",
            "--format",
            "json",
            "--check",
            "--progress",
            "never",
        )
        if failed_cli.returncode != 1:
            raise AssertionError(f"failing recipe exit={failed_cli.returncode}")
        passed_cli = run_cli(
            repository,
            "run",
            "max-file-bytes",
            str(root),
            "--max",
            "1MiB",
            "--format",
            "json",
            "--check",
            "--progress",
            "never",
        )
        if passed_cli.returncode or json.loads(passed_cli.stdout)["summary"]["blocking"]:
            raise AssertionError(passed_cli.stderr.decode(errors="replace"))

        configured = root / "configured"
        configured.mkdir()
        (configured / "src").mkdir()
        (configured / "src/small.py").write_text("x = 1\n", encoding="utf-8")
        (configured / "src/large.py").write_text("x" * 20, encoding="utf-8")
        (configured / "archbird.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "project": "recipe-configured",
                    "limits": {"max_file_bytes": 100},
                    "layers": [
                        {
                            "name": "python",
                            "role": "core",
                            "language": "python",
                            "globs": ["src/**/*.py"],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        configured_request = request(include=["src/**"])
        configured_request["project"] = {"config": "archbird.json"}
        configured_suite = extension.verification_recipe_compile(
            encoded(configured_request), False
        )
        suite_path = configured / "archbird.verify.json"
        suite_path.write_bytes(configured_suite)
        configured_result = Verification.from_config(suite_path).result()
        if (
            configured_result["checks"][0]["status"] != "fail"
            or [
                row["key"]
                for row in configured_result["checks"][0]["findings"]
            ]
            != ["src/large.py"]
            or configured_result["projects"][0]["capabilities"]
        ):
            raise AssertionError(
                f"configured inventory verification is incomplete: "
                f"{configured_result!r}"
            )

    print("verification recipe contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
