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


def assert_callback_entry_interrupt(repository: Path, fixture: Path, environment):
    # Deliver SIGINT before the ctypes callback's first statement, where its
    # local exception handler cannot catch Python's default signal handler.
    probe = r"""
import os, signal, sys
from archbird import _native
from archbird.cli import main

fired = False
def interrupt(frame, event, arg):
    global fired
    if not fired and event == 'call' and frame.f_code.co_name == '_python_cancel':
        fired = True
        os.kill(os.getpid(), signal.SIGINT)
    return interrupt

previous = signal.getsignal(signal.SIGINT)
sys.settrace(interrupt)
try:
    result = main(['map', sys.argv[1], '--no-config', '--no-cache',
                   '--max-file-bytes', '8000000', '--progress', 'never'])
finally:
    sys.settrace(None)
assert fired, 'native cancellation callback was not reached'
assert result == 130, result
assert signal.getsignal(signal.SIGINT) is previous
assert _native.json_canonicalize(b'{"ok":true}') == b'{"ok":true}'
signal.signal(signal.SIGINT, signal.SIG_IGN)
assert _native.json_canonicalize(b'{}') == b'{}'
assert signal.getsignal(signal.SIGINT) == signal.SIG_IGN
"""
    completed = subprocess.run(
        [sys.executable, "-c", probe, str(fixture)],
        cwd=repository, env=environment, capture_output=True, text=True, timeout=20,
    )
    assert completed.returncode == 0, completed.stderr
    assert not completed.stdout, completed.stdout
    assert completed.stderr == "archbird: cancelled\n", completed.stderr


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
        assert_callback_entry_interrupt(repository, fixture, environment)
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
