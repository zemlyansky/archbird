#!/usr/bin/env python3
"""Validate and print Archbird's public distribution version."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def _match(path: str, pattern: str) -> str:
    text = (ROOT / path).read_text(encoding="utf-8")
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"cannot read Archbird version from {path}")
    return match.group(1)


def versions() -> dict[str, str]:
    return {
        "cmake": _match(
            "CMakeLists.txt",
            r"^project\(Archbird VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$",
        ),
        "core-fallback": _match(
            "src/base/archbird_internal.h",
            r'^#define ARCHBIRD_VERSION "([^"]+)"$',
        ),
        "node-binding-fallback": _match(
            "bindings/node.c",
            r'^#define ARCHBIRD_VERSION "([^"]+)"$',
        ),
        "wasm-binding-fallback": _match(
            "bindings/wasm.c",
            r'^#define ARCHBIRD_VERSION "([^"]+)"$',
        ),
        "python": _match("py/pyproject.toml", r'^version = "([^"]+)"$'),
        "python-runtime": _match(
            "py/archbird/__init__.py", r'^__version__ = "([^"]+)"$'
        ),
        "node": json.loads((ROOT / "js/package.json").read_text(encoding="utf-8"))[
            "version"
        ],
        "node-lock": json.loads(
            (ROOT / "js/package-lock.json").read_text(encoding="utf-8")
        )["packages"][""]["version"],
        "node-lock-root": json.loads(
            (ROOT / "js/package-lock.json").read_text(encoding="utf-8")
        )["version"],
    }


def validate(values: dict[str, str]) -> str:
    unique = set(values.values())
    if len(unique) != 1:
        details = ", ".join(f"{name}={value}" for name, value in values.items())
        raise RuntimeError(f"Archbird public versions differ: {details}")
    return next(iter(unique))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--print",
        choices=("native", "python", "node"),
        dest="target",
    )
    args = parser.parse_args()
    values = versions()
    version = validate(values)
    if args.target is not None:
        print(values[{"native": "cmake"}.get(args.target, args.target)])
    else:
        print(f"Archbird public version {version}")


if __name__ == "__main__":
    main()
