#!/usr/bin/env python3
"""Differential workspace joins over canonical saved maps."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.render import _map_dict, _workspace_dict
from archbird.map.workspace import analyze_workspace, load_workspace_config


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_workspace.py EXTENSION WORKSPACE_CONFIG")
    extension = load_extension(Path(sys.argv[1]).resolve())
    config_path = Path(sys.argv[2]).resolve()
    config_json = config_path.read_bytes()
    oracle = analyze_workspace(load_workspace_config(config_path))
    maps = [_map_dict(project) for project in oracle.projects.values()]

    plan = json.loads(extension.workspace_plan(config_json))
    assert plan["workspace"] == oracle.name
    assert plan["description"] == oracle.description
    assert plan["config_sha256"] == oracle.config_digest
    assert len(plan["projects"]) == len(maps)

    native = json.loads(extension.workspace_analyze(config_json, canonical(maps)))
    expected = _workspace_dict(oracle)
    native.pop("tool")
    expected.pop("tool")
    if native != expected:
        raise AssertionError(
            "native workspace differs\n"
            + json.dumps(
                {"native": native, "expected": expected},
                indent=2,
                sort_keys=True,
            )
        )
    first = extension.workspace_analyze(config_json, canonical(maps))
    second = extension.workspace_analyze(config_json, canonical(list(reversed(maps))))
    assert first == second

    duplicate = copy.deepcopy(
        next(item for item in maps if item["project"] == "dependency")
    )
    duplicate["project"] = duplicate["project"] + "-copy"
    ambiguous_config = json.loads(config_json)
    ambiguous_config["workspace"] = "ambiguous"
    ambiguous_config["projects"].append({"config": "copy/archbird.json"})
    ambiguous = json.loads(
        extension.workspace_analyze(
            canonical(ambiguous_config), canonical([*maps, duplicate])
        )
    )
    assert not ambiguous["routes"]
    assert {
        row["code"] for row in ambiguous["diagnostics"]
    } == {"workspace-package-ambiguous"}

    print(
        "native workspace parity passed: "
        f"{len(maps)} projects, {len(native['routes'])} routes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
