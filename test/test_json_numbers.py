#!/usr/bin/env python3
"""Differential finite-binary64 JSON formatting against CPython."""

from __future__ import annotations

import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys


CASE_COUNT = 100_000
SEED = 0xA7CB1D


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_json_numbers.py JSON_CLI")
    executable = Path(sys.argv[1]).resolve()
    randomizer = random.Random(SEED)
    values: list[float] = []
    while len(values) < CASE_COUNT:
        raw = randomizer.getrandbits(64).to_bytes(8, "big")
        value = struct.unpack(">d", raw)[0]
        if math.isfinite(value):
            values.append(value)
    expected = json.dumps(
        values,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")
    completed = subprocess.run(
        [str(executable)],
        input=expected,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise AssertionError(completed.stderr.decode("utf-8", errors="replace"))
    if completed.stdout != expected:
        mismatch = next(
            (
                index
                for index, (left, right) in enumerate(
                    zip(completed.stdout, expected)
                )
                if left != right
            ),
            min(len(completed.stdout), len(expected)),
        )
        raise AssertionError(
            "native/Python real formatting differs at byte "
            f"{mismatch}: native={completed.stdout[mismatch:mismatch + 80]!r} "
            f"python={expected[mismatch:mismatch + 80]!r}"
        )
    print(
        f"finite JSON real formatting matches CPython: cases={CASE_COUNT} "
        f"seed={SEED:#x} bytes={len(expected)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
