#!/usr/bin/env python3
"""Prove that the Python live host starts before initial Map analysis."""

from __future__ import annotations

import shutil
import sys
import threading
import time
from pathlib import Path

from archbird.serve import LiveRepository, create_live_server


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_python_live_repository.py APP_ROOT FIXTURE_ROOT TEMP_ROOT"
        )
    app_root = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    temporary = Path(sys.argv[3]).resolve()
    repository = temporary / "repository"
    shutil.rmtree(temporary, ignore_errors=True)
    shutil.copytree(fixture, repository)

    original = LiveRepository._candidate
    release = threading.Event()
    candidate_calls = 0

    def delayed(self: LiveRepository):
        nonlocal candidate_calls
        candidate_calls += 1
        if not release.wait(10):
            raise RuntimeError("test candidate gate timed out")
        return original(self)

    LiveRepository._candidate = delayed
    server = None
    try:
        started = time.monotonic()
        server = create_live_server(app=app_root, port=0, root=repository)
        elapsed = time.monotonic() - started
        if elapsed >= 0.25:
            raise AssertionError(f"live server waited {elapsed:.3f}s for initial analysis")
        state = server.repository.state()
        if state["source_available"] or state["phase"] not in {"waiting", "analyzing"}:
            raise AssertionError(f"unexpected initial live state: {state!r}")
        release.set()
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            state = server.repository.state()
            if state["phase"] == "ready":
                break
            time.sleep(0.025)
        else:
            raise AssertionError(f"initial live candidate did not become ready: {state!r}")
        if state["project"] != "map-base" or not state["source_available"]:
            raise AssertionError(f"invalid ready state: {state!r}")
        time.sleep(0.8)
        if candidate_calls != 1:
            raise AssertionError(
                f"unchanged repository triggered {candidate_calls} candidate analyses"
            )
        print("Python live server starts before initial Map analysis")
        return 0
    finally:
        release.set()
        LiveRepository._candidate = original
        if server is not None:
            server.close()


if __name__ == "__main__":
    raise SystemExit(main())
