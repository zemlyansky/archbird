#!/usr/bin/env python3
"""Python CLI progress is opt-in off-TTY and never contaminates artifacts."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


class _TerminalCapture:
    def __init__(self) -> None:
        self.value = ""

    def isatty(self) -> bool:
        return True

    def write(self, value: str) -> int:
        self.value += value
        return len(value)

    def flush(self) -> None:
        return None


def verify_adaptive_terminal() -> None:
    """Exercise the delayed, single-line TTY projection without sleeping."""

    import archbird.cli as cli

    capture = _TerminalCapture()
    ticks = iter((0.0, 0.1, 1.1, 1.3, 1.4))
    original_stderr = cli.sys.stderr
    original_monotonic = cli.time.monotonic
    try:
        cli.sys.stderr = capture
        cli.time.monotonic = lambda: next(ticks)
        progress = cli._Progress("auto")
        progress.emit({"phase": "providers", "provider": "empty", "state": "start", "total": 0})
        progress.emit(
            {
                "phase": "providers",
                "provider": "syntax:tree-sitter:javascript",
                "state": "progress",
                "completed": 5,
                "total": 10,
            }
        )
        progress.emit({"phase": "joining", "state": "start"})
        progress.finish()
    finally:
        cli.time.monotonic = original_monotonic
        cli.sys.stderr = original_stderr
    if "empty" in capture.value or "0/0" in capture.value:
        raise AssertionError(capture.value)
    if "5/10 files (50%)" not in capture.value:
        raise AssertionError(capture.value)
    if not capture.value.startswith("\r") or capture.value.count("\n") != 1:
        raise AssertionError(f"TTY progress was not one updating line: {capture.value!r}")


def run(root: Path, mode: str) -> subprocess.CompletedProcess[bytes]:
    fixture = root / "test/fixtures/map_base"
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "archbird",
            "map",
            str(fixture),
            "--config",
            str(fixture / "archbird.json"),
            "--progress",
            mode,
            "--no-cache",
            "--format",
            "json",
            "--check",
        ],
        check=True,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_cli_progress.py REPOSITORY")
    root = Path(sys.argv[1]).resolve()
    always = run(root, "always")
    automatic = run(root, "auto")
    if always.stdout != automatic.stdout:
        raise AssertionError("progress mode changed canonical Map bytes")
    document = json.loads(always.stdout)
    if document["project"] != "map-base":
        raise AssertionError(document["project"])
    progress = always.stderr.decode("utf-8")
    for phase in ("discovery", "selected", "providers", "joining", "rendering", "complete"):
        if f"] {phase}:" not in progress:
            raise AssertionError(f"missing {phase} phase: {progress}")
    if automatic.stderr:
        raise AssertionError(f"auto progress wrote off-TTY: {automatic.stderr!r}")
    verify_adaptive_terminal()
    print("Python CLI progress isolation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
