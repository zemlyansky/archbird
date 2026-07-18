#!/usr/bin/env python3
"""Internal native repository Map driver used by differential/profile tests."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: map_repository_cli.py EXTENSION NATIVE_ROOT CONFIG ROOT"
        )
    extension = Path(sys.argv[1]).resolve()
    native_root = Path(sys.argv[2]).resolve()
    sys.path.insert(0, str(native_root))
    import archbird

    spec = importlib.util.spec_from_file_location("archbird._native", extension)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {extension}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    from archbird.native import Project

    project = Project.from_config(sys.argv[3], root=sys.argv[4])
    sys.stdout.buffer.write(project.map_json())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
