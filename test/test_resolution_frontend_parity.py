#!/usr/bin/env python3
"""Require Python and Node hosts to emit identical discovery resolution."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_resolution_frontend_parity.py "
            "PY_EXTENSION NODE ADDON REPOSITORY"
        )
    extension, node, addon, repository = map(Path, sys.argv[1:])
    repository = repository.resolve()
    fixture = repository / "test/fixtures/zero_config"
    sys.path.insert(0, str(repository / "py"))
    load_extension(extension.resolve())
    from archbird.native import Project, resolve_discovery

    python_outputs = [
        resolve_discovery(fixture),
        resolve_discovery(
            fixture,
            project="cli",
            ignore_files=(".customignore",),
            max_file_bytes=100,
            max_index_bytes=1000,
        ),
        resolve_discovery(
            fixture,
            ignore=False,
            ignore_files=(".customignore",),
        ),
    ]
    completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(fixture),
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    node_outputs = [bytes.fromhex(row) for row in completed.stdout.decode().splitlines()]
    if node_outputs != python_outputs:
        raise AssertionError("Python and Node config-resolution artifacts differ")
    r_fixture = repository / "test/fixtures/zero_config_r"
    r_resolution = resolve_discovery(r_fixture)
    r_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(r_fixture),
            "default",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if bytes.fromhex(r_completed.stdout.decode().strip()) != r_resolution:
        raise AssertionError("Python and Node CRAN resolution artifacts differ")
    r_project = Project.from_repository(r_fixture, jobs=1)
    r_map = json.loads(r_project.map_json())
    if r_map["project"] != "zeroR":
        raise AssertionError(f"DESCRIPTION identity was lost: {r_map['project']!r}")
    if len(r_map["packages"]) != 1:
        raise AssertionError(f"CRAN package was not inferred: {r_map['packages']!r}")
    r_package = r_map["packages"][0]
    if (
        r_package["identity"] != "zeroR"
        or r_package["version"] != "1.2.3"
        or r_package["manifest"] != "DESCRIPTION"
        or r_package["exports"] != ["alpha", "beta"]
    ):
        raise AssertionError(f"CRAN package evidence is incorrect: {r_package!r}")
    print("Python/Node config-resolution parity passed for npm, Python, and CRAN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
