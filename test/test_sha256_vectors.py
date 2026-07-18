#!/usr/bin/env python3
"""Run the pinned NIST CAVP SHA-256 byte-oriented message vectors."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


def parse_vectors(path: Path):
    row = {}
    for raw_line in path.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("["):
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        row[key] = value
        if key == "MD":
            length = int(row["Len"])
            message = b"" if length == 0 else bytes.fromhex(row["Msg"])
            if len(message) * 8 != length:
                raise AssertionError(f"non-byte vector in {path}: {length}")
            yield length, message, value.lower()
            row = {}


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_sha256_vectors.py HASH_CLI")
    root_raw = os.environ.get("NIST_SHA256_ROOT")
    if not root_raw:
        raise SystemExit("NIST_SHA256_ROOT must name the extracted NIST vector directory")
    root = Path(root_raw)
    executable = Path(sys.argv[1]).resolve()
    count = 0
    for name in ("SHA256ShortMsg.rsp", "SHA256LongMsg.rsp"):
        for length, message, expected in parse_vectors(root / name):
            completed = subprocess.run(
                [str(executable)], input=message, capture_output=True, check=True
            )
            actual = completed.stdout.decode("ascii").strip()
            if actual != expected:
                raise AssertionError(
                    f"{name} Len={length}: expected {expected}, got {actual}"
                )
            count += 1
    print(f"NIST SHA-256 vectors passed: {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
