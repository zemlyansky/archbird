#!/usr/bin/env python3
"""Python CLI progress is opt-in off-TTY and never contaminates artifacts."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


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


def run(
    root: Path,
    mode: str,
    *,
    cache_dir: Path | None = None,
    cache_max_bytes: int | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[bytes]:
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
            "--python-provider-timeout",
            "30",
            *(
                ["--no-cache"]
                if cache_dir is None
                else ["--cache-dir", str(cache_dir)]
            ),
            *(
                []
                if cache_max_bytes is None
                else ["--cache-max-bytes", str(cache_max_bytes)]
            ),
            "--format",
            "json",
            *(["--check"] if check else []),
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
    streamed = run(root, "auto", check=False)
    if always.stdout != automatic.stdout:
        raise AssertionError("progress mode changed canonical Map bytes")
    if streamed.stdout != always.stdout:
        raise AssertionError("streaming changed canonical Map bytes")
    document = json.loads(always.stdout)
    if document["project"] != "map-base":
        raise AssertionError(document["project"])
    progress = always.stderr.decode("utf-8")
    for phase in ("discovery", "selected", "providers", "joining", "rendering", "complete"):
        if f"] {phase}:" not in progress:
            raise AssertionError(f"missing {phase} phase: {progress}")
    if automatic.stderr:
        raise AssertionError(f"auto progress wrote off-TTY: {automatic.stderr!r}")
    invalid_timeout = subprocess.run(
        [
            sys.executable,
            "-m",
            "archbird",
            "map",
            str(root / "test/fixtures/map_base"),
            "--no-config",
            "--python-provider-timeout",
            "0",
        ],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if (
        invalid_timeout.returncode != 2
        or b"--python-provider-timeout must be finite and positive"
        not in invalid_timeout.stderr
    ):
        raise AssertionError(
            f"invalid provider timeout was not rejected: {invalid_timeout!r}"
        )
    cache_parent = root / "build"
    cache_parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache_parent) as raw:
        cache_root = Path(raw) / "cache"
        cached = run(root, "never", cache_dir=cache_root, check=False)
        if cached.stdout != always.stdout or not tuple(
            (cache_root / "maps-v1").rglob("*.json")
        ):
            raise AssertionError("unchecked streaming CLI did not populate Map cache")
        bounded = run(
            root,
            "never",
            cache_dir=Path(raw) / "bounded",
            cache_max_bytes=1,
            check=False,
        )
        if (
            bounded.stdout != always.stdout
            or b"canonical Map exceeded the configured cache budget"
            not in bounded.stderr
        ):
            raise AssertionError("streaming CLI did not report bounded Map cache")
    verify_adaptive_terminal()
    print("Python CLI progress isolation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
