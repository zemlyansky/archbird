#!/usr/bin/env python3
"""Prove that every structured fuzz seed reaches its intended native API."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def required_bytes(path: Path) -> bytes:
    value = path.read_bytes()
    if not value:
        raise AssertionError(f"empty fuzz seed: {path}")
    return value


def artifact(raw: bytes, expected: str) -> dict[str, object]:
    document = json.loads(raw)
    if document.get("artifact") != expected:
        raise AssertionError(
            f"expected artifact={expected!r}, got {document.get('artifact')!r}"
        )
    return document


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_fuzz_seeds.py EXTENSION CORPUS")
    native = load_extension(Path(sys.argv[1]).resolve())
    root = Path(sys.argv[2]).resolve()

    map_json = required_bytes(root / "map" / "minimal.json")
    act_map_json = required_bytes(root / "act-map" / "map.json")
    query_json = required_bytes(root / "query" / "request.json")
    workspace_json = required_bytes(root / "workspace" / "config.json")
    workspace_maps = required_bytes(root / "workspace-maps" / "maps.json")
    verification = required_bytes(root / "verification" / "verification.json")

    artifact(native.map_query(map_json, query_json), "query")
    artifact(native.map_diff(map_json, map_json), "diff")
    artifact(native.map_freshness(map_json, map_json), "map-freshness")
    for format_name in ("graphml", "mermaid"):
        for view in ("components", "files"):
            rendered = native.map_export_graph(
                map_json,
                format_name,
                view,
                direction="LR",
                max_nodes=200,
                max_edge_names=3,
            )
            if not rendered:
                raise AssertionError(f"empty {format_name}/{view} projection")

    plan = json.loads(native.workspace_plan(workspace_json))
    if plan.get("workspace") != "fuzz":
        raise AssertionError("workspace seed did not reach a valid plan")
    artifact(native.workspace_analyze(workspace_json, workspace_maps), "workspace")

    artifact(verification, "verification")
    for verification_value in (b"", verification):
        artifact(
            native.okf_publish(
                act_map_json,
                verification_value,
                b"",
                pretty=False,
            ),
            "okf-output-bundle",
        )

    print("structured Map, workspace, report, graph, and OKF seeds passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
