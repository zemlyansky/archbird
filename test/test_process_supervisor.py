#!/usr/bin/env python3
"""Adversarial liveness tests for Python host process supervision."""

from __future__ import annotations

import multiprocessing
import os
from pathlib import Path
import shutil
import sys
import time
from typing import Tuple

from archbird._process_supervisor import supervised_ordered_map


Task = Tuple[str, float, str]


def worker(task: Task) -> str:
    action, delay, value = task
    if action == "record-hang":
        Path(value).write_text(str(os.getpid()), encoding="ascii")
        time.sleep(delay)
    elif action in ("record-exit", "record-clean-exit"):
        Path(value).write_text(str(os.getpid()), encoding="ascii")
        os._exit(0 if action == "record-clean-exit" else 17)
    elif action == "error":
        raise RuntimeError(value)
    else:
        time.sleep(delay)
    return value


def assert_no_active_child(pid: int) -> None:
    if any(child.pid == pid for child in multiprocessing.active_children()):
        raise AssertionError(f"worker process {pid} survived supervisor cleanup")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_process_supervisor.py WORK_DIRECTORY")
    requested_start_method = os.environ.get("ARCHBIRD_TEST_START_METHOD")
    if requested_start_method:
        multiprocessing.set_start_method(requested_start_method)
    work = Path(sys.argv[1]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    ordered = tuple(
        supervised_ordered_map(
            worker,
            (
                ("return", 0.15, "first"),
                ("return", 0.01, "second"),
                ("return", 0.02, "third"),
            ),
            workers=2,
            timeout_seconds=2.0,
            describe=lambda task: task[2],
        )
    )
    if ordered != ("first", "second", "third"):
        raise AssertionError(f"worker completion changed input order: {ordered!r}")

    try:
        tuple(
            supervised_ordered_map(
                worker,
                (("error", 0.0, "deliberate worker failure"),),
                workers=1,
                timeout_seconds=2.0,
                describe=lambda task: task[2],
            )
        )
    except RuntimeError as error:
        if "deliberate worker failure" not in str(error):
            raise
    else:
        raise AssertionError("worker exception was not propagated")

    timeout_pid = work / "timeout.pid"
    started = time.monotonic()
    try:
        tuple(
            supervised_ordered_map(
                worker,
                (("record-hang", 60.0, str(timeout_pid)),),
                workers=1,
                timeout_seconds=0.5,
                describe=lambda task: task[2],
            )
        )
    except TimeoutError as error:
        if str(timeout_pid) not in str(error):
            raise AssertionError(f"timeout omitted task identity: {error}") from error
    else:
        raise AssertionError("non-terminating worker did not time out")
    elapsed = time.monotonic() - started
    if elapsed > 5.0:
        raise AssertionError(f"worker termination was not bounded: {elapsed:.3f}s")
    assert_no_active_child(int(timeout_pid.read_text(encoding="ascii")))

    exit_pid = work / "exit.pid"
    started = time.monotonic()
    try:
        tuple(
            supervised_ordered_map(
                worker,
                (("record-exit", 0.0, str(exit_pid)),),
                workers=1,
                timeout_seconds=10.0,
                describe=lambda task: task[2],
            )
        )
    except RuntimeError as error:
        if "exited with code 17" not in str(error):
            raise AssertionError(f"worker exit was not diagnosed: {error}") from error
    else:
        raise AssertionError("hard-exited worker was not diagnosed")
    elapsed = time.monotonic() - started
    if elapsed > 5.0:
        raise AssertionError(f"worker exit detection was not bounded: {elapsed:.3f}s")
    assert_no_active_child(int(exit_pid.read_text(encoding="ascii")))

    clean_exit_pid = work / "clean-exit.pid"
    started = time.monotonic()
    try:
        tuple(
            supervised_ordered_map(
                worker,
                (("record-clean-exit", 0.0, str(clean_exit_pid)),),
                workers=1,
                timeout_seconds=10.0,
                describe=lambda task: task[2],
            )
        )
    except RuntimeError as error:
        if "exited with code 0" not in str(error):
            raise AssertionError(
                f"clean worker exit was not diagnosed: {error}"
            ) from error
    else:
        raise AssertionError("clean-exited worker was not diagnosed")
    elapsed = time.monotonic() - started
    if elapsed > 5.0:
        raise AssertionError(
            f"clean worker exit detection was not bounded: {elapsed:.3f}s"
        )
    assert_no_active_child(int(clean_exit_pid.read_text(encoding="ascii")))

    cancel_pid = work / "cancel.pid"
    results = supervised_ordered_map(
        worker,
        (
            ("return", 0.0, "ready"),
            ("record-hang", 60.0, str(cancel_pid)),
        ),
        workers=2,
        timeout_seconds=30.0,
        describe=lambda task: task[2],
    )
    if next(results) != "ready":
        raise AssertionError("unexpected first supervised result")
    deadline = time.monotonic() + 2.0
    while not cancel_pid.exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    if not cancel_pid.exists():
        raise AssertionError("cancellation fixture worker did not start")
    results.close()
    assert_no_active_child(int(cancel_pid.read_text(encoding="ascii")))

    print("process supervisor tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
