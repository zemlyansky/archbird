#!/usr/bin/env python3
"""Clean-install smoke test for the PyPI distribution."""

from __future__ import annotations

import argparse
import hashlib
import json
import importlib.machinery
from importlib.metadata import version
import os
from pathlib import Path
import subprocess
import sys

import archbird
from archbird import native
from release_streaming_contract import validate_streaming_contract


def _inside(child: Path, parent: Path) -> bool:
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def _project(config: Path, root: Path) -> archbird.Project:
    return archbird.Project.from_config(
        config,
        root=root,
        cache_dir=None,
        map_cache=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("config")
    parser.add_argument("root")
    parser.add_argument("--output")
    args = parser.parse_args()
    config = Path(args.config).resolve()
    root = Path(args.root).resolve()
    config_json = config.read_bytes()
    package_root = Path(archbird.__file__).resolve().parent
    if not _inside(package_root, Path(sys.prefix).resolve()):
        raise AssertionError(
            f"release smoke imported Archbird outside its venv: {package_root}"
        )
    import archbird._native as compiled_native

    compiled_path = Path(compiled_native.__file__).resolve()
    if not _inside(compiled_path, package_root) or not any(
        str(compiled_path).endswith(suffix)
        for suffix in importlib.machinery.EXTENSION_SUFFIXES
    ):
        raise AssertionError(
            f"release smoke did not load the installed extension: {compiled_path}"
        )
    if archbird.__version__ != version("archbird"):
        raise AssertionError(f"unexpected version: {archbird.__version__}")
    if archbird.PATTERN_CONTRACT_VERSION != 1:
        raise AssertionError("unexpected configured-pattern contract version")
    if archbird.PATTERN_CONTRACT != "archbird-pcre2-v1":
        raise AssertionError("unexpected configured-pattern contract")
    if archbird.PATTERN_ENGINE != "PCRE2 10.47":
        raise AssertionError("unexpected configured-pattern engine")
    if archbird.PATTERN_UNICODE != "UCD 16.0.0":
        raise AssertionError("unexpected configured-pattern Unicode data")
    if archbird.PATTERN_OPTIONS != (
        "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,"
        "NEVER_CALLOUT,JIT_DISABLED"
    ):
        raise AssertionError("unexpected configured-pattern options")
    compiled_config = json.loads(archbird.compile_project_configuration(config_json))
    if compiled_config["artifact"] != "project-configuration-plan":
        raise AssertionError("installed Python configuration compiler failed")
    project = _project(config, root)
    if len(project.map_input_sha256) != 64:
        raise AssertionError("installed Python host returned an invalid Map-input digest")

    first = validate_streaming_contract(lambda: _project(config, root))

    environment = {
        key: value
        for key, value in os.environ.items()
        if key
        not in {
            "ARCHBIRD_CACHE_DIR",
            "ARCHBIRD_LIBRARY",
            "PYTHONHOME",
            "PYTHONPATH",
        }
    }
    cli = subprocess.run(
        [
            sys.executable,
            "-m",
            "archbird",
            "map",
            "--config",
            str(config),
            "--root",
            str(root),
            "--format",
            "json",
            "--no-cache",
        ],
        cwd=root,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if cli.returncode != 0 or not cli.stdout:
        raise AssertionError(
            "installed unchecked JSON CLI failed: "
            f"status={cli.returncode}\n"
            f"stdout={cli.stdout!r}\n"
            f"stderr={cli.stderr.decode('utf-8', errors='replace')}"
        )
    if json.loads(cli.stdout)["artifact"] != "map":
        raise AssertionError("installed unchecked JSON CLI returned the wrong artifact")

    if first != project.map_json():
        raise AssertionError("installed Python Map is not deterministic")
    document = json.loads(first)
    if document["artifact"] != "map" or document["project"] != "map-base":
        raise AssertionError("installed Python package returned the wrong map")
    if any(row["severity"] == "error" for row in document["diagnostics"]):
        raise AssertionError(document["diagnostics"])
    freshness = json.loads(archbird.audit_map_freshness(first, first))
    if freshness["status"] != "current":
        raise AssertionError("installed Python freshness audit failed")
    query = project.query(paths=["py/pkg"], depth=0)
    if query["artifact"] != "query" or len(query["files"]) != 2:
        raise AssertionError("installed Python query failed")
    projection = json.loads(
        archbird.evaluate_projection_json(
            first,
            {
                "id": "release-symbols",
                "select": "symbols",
                "paths": ["js/index.js"],
            },
        )
    )
    if (
        projection["artifact"] != "projection-result"
        or not projection["completeness"]["exhaustive"]
        or [item["key"] for item in projection["fact"]["items"]]
        != ["add", "twice"]
    ):
        raise AssertionError("installed Python projection failed")
    query_plan = json.loads(
        archbird.compile_query_plan_json(
            b"",
            overrides={"paths": ["py/pkg"], "depth": 0},
        )
    )
    if (
        query_plan["artifact"] != "query-plan"
        or query_plan["schema_version"] != 3
        or query_plan["plan"]["kind"] != "ad_hoc"
        or query_plan["plan"]["selection"]["paths"] != ["py/pkg"]
        or "projection_result_sha256"
        in query_plan["plan"]["projections"][0]
    ):
        raise AssertionError("installed Python QueryPlan compiler failed")
    verification = json.loads(
        archbird.evaluate_constraints_json(config_json, first)
    )
    if verification["artifact"] != "verification" or len(
        verification["constraints"]
    ) != 3:
        raise AssertionError("installed Python constraint evaluation failed")
    sarif = json.loads(
        archbird.evaluate_constraints_json(config_json, first, format="sarif")
    )
    if sarif["version"] != "2.1.0" or len(sarif["runs"]) != 1:
        raise AssertionError("installed Python constraint report failed")
    baseline = json.loads(
        archbird.freeze_constraints_json(
            config_json,
            first,
            owner="release",
            rationale="Exercise the installed extension boundary.",
        )
    )
    if baseline["artifact"] != "constraint-baseline":
        raise AssertionError("installed Python constraint freeze failed")
    graph = json.loads(project.graph_view_json())
    symbols = json.loads(
        project.graph_view_json(
            view="symbols",
            query={"symbols": ["js/index.js:add"], "depth": 1},
        )
    )
    if graph["artifact"] != "archbird-graph-view" or graph["source"]["artifact"] != "map":
        raise AssertionError("installed Python component graph failed")
    if symbols["request"]["view"] != "symbols" or symbols["source"]["artifact"] != "query":
        raise AssertionError("installed Python symbol graph failed")
    report = {
        "artifact": "archbird-python-release-conformance",
        "extension": str(compiled_path),
        "implementation_sha256": native.CORE_IMPLEMENTATION_SHA256,
        "map_sha256": hashlib.sha256(first).hexdigest(),
        "operations": [
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
        ],
        "package": str(package_root),
        "version": archbird.__version__,
    }
    if args.output:
        Path(args.output).write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(
        f"python release smoke passed: files={len(document['files'])} "
        f"symbols={sum(len(row['symbols']) for row in document['files'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
