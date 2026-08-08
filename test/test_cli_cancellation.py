#!/usr/bin/env python3
"""Prove SIGINT cooperatively stops an in-process native syntax provider."""

from __future__ import annotations

import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile


def main() -> int:
    repository = Path(sys.argv[1]).resolve()
    temporary_root = repository / "build/tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="cli-cancel-", dir=temporary_root
    ) as raw:
        fixture = Path(raw)
        source = fixture / "large.js"
        source.write_bytes(b"const value=0;\n" * 400_000)
        environment = {
            **os.environ,
            "ARCHBIRD_LIB": str(repository / "build/libarchbird.so"),
            "PYTHONPATH": str(repository / "py"),
            "TMPDIR": str(temporary_root),
        }
        process = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "archbird",
                "map",
                str(fixture),
                "--no-config",
                "--no-cache",
                "--max-file-bytes",
                "8000000",
                "--progress",
                "always",
                "--format",
                "json",
            ],
            cwd=repository,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        observed: list[str] = []
        assert process.stderr is not None
        deadline = 20
        while deadline > 0:
            ready, _, _ = select.select([process.stderr], [], [], 1.0)
            deadline -= 1
            if not ready:
                if process.poll() is not None:
                    break
                continue
            line = process.stderr.readline()
            if not line:
                break
            observed.append(line)
            if "syntax:tree-sitter:javascript started" in line:
                process.send_signal(signal.SIGINT)
                break
        else:
            process.kill()
            raise AssertionError("Tree-sitter progress was not reached")
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            raise AssertionError(
                "Tree-sitter progress was not reached before exit "
                f"{process.returncode}: {stdout!r} {''.join(observed) + stderr!r}"
            )
        try:
            stdout, stderr = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise AssertionError("SIGINT did not stop native provider work")
        diagnostic = "".join(observed) + stderr
        if process.returncode != 130:
            raise AssertionError(
                f"cancelled CLI exited {process.returncode}: {diagnostic}"
            )
        if stdout or "archbird: cancelled" not in diagnostic:
            raise AssertionError(
                f"cancelled CLI emitted an invalid result: {stdout!r} {diagnostic!r}"
            )
        if "Traceback" in diagnostic:
            raise AssertionError(f"cancelled CLI leaked a traceback: {diagnostic}")
    print("CLI cooperative cancellation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
