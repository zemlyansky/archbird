#!/usr/bin/env python3
"""Require Python and Node hosts to emit identical discovery resolution."""

from __future__ import annotations

import importlib.util
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
    from archbird.native import resolve_discovery

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
    print("Python/Node config-resolution parity passed for three request modes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
