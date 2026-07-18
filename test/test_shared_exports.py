#!/usr/bin/env python3
"""Require the shared-library ABI to equal the reviewed public header."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path


PUBLIC = re.compile(
    r"ARCHBIRD_API\s+(?:ArchbirdStatus|void|size_t|const char\s*\*)\s*"
    r"(archbird_[A-Za-z0-9_]+)\s*\(",
    re.MULTILINE,
)


def public_symbols(header: Path) -> set[str]:
    text = header.read_text(encoding="utf-8")
    names = PUBLIC.findall(text)
    if not names or len(names) != len(set(names)):
        raise RuntimeError("public header API declarations are empty or duplicated")
    return set(names)


def dynamic_symbols(library: Path) -> set[str]:
    if sys.platform == "darwin":
        command = ["nm", "-gU", str(library)]
    elif sys.platform == "win32":
        dumpbin = shutil.which("dumpbin")
        if dumpbin is None:
            raise RuntimeError("dumpbin is required for the Windows ABI export gate")
        output = subprocess.check_output(
            [dumpbin, "/nologo", "/exports", str(library)], text=True
        )
        return {
            match.group(1)
            for match in re.finditer(
                r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)",
                output,
                re.MULTILINE,
            )
        }
    else:
        command = ["nm", "-D", "--defined-only", str(library)]
    output = subprocess.check_output(command, text=True)
    names = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[-1].split("@", 1)[0]
        if sys.platform == "darwin" and name.startswith("_archbird_"):
            name = name[1:]
        if re.fullmatch(r"archbird_[A-Za-z0-9_]+", name):
            names.add(name)
        elif name and not name.startswith(("_init", "_fini")):
            names.add(name)
    return names


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_shared_exports.py HEADER SHARED_LIBRARY")
    expected = public_symbols(Path(sys.argv[1]))
    actual = dynamic_symbols(Path(sys.argv[2]))
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        raise RuntimeError(
            f"shared ABI mismatch: missing={missing}, unexpected={unexpected}"
        )
    print(f"archbird shared ABI: {len(actual)} reviewed symbols")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
