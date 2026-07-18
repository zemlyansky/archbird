#!/usr/bin/env python3
"""Compatibility wrapper for the shared deterministic C snapshot builder."""

from __future__ import annotations

from pathlib import Path
import sys


REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from tools.sync_csrc import sync  # noqa: E402


if __name__ == "__main__":
    print(sync("python"))
