"""Bounded, deterministic supervision for Python host worker processes."""

from __future__ import annotations

from dataclasses import dataclass
import math
import multiprocessing
from multiprocessing.connection import Connection, wait
import signal
import time
from typing import Callable, Dict, Iterable, Iterator, Optional, TypeVar


_Input = TypeVar("_Input")
_Output = TypeVar("_Output")


def _worker_main(connection: Connection, function: Callable[[object], object]) -> None:
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        while True:
            command = connection.recv()
            if command is None:
                return
            index, item = command
            try:
                value = function(item)
            except BaseException as error:
                connection.send(
                    (
                        "error",
                        index,
                        type(error).__module__,
                        type(error).__name__,
                        str(error),
                    )
                )
            else:
                connection.send(("result", index, value))
    except (EOFError, BrokenPipeError, OSError):
        return
    finally:
        connection.close()


@dataclass
class _Worker:
    process: multiprocessing.Process
    connection: Connection
    index: Optional[int] = None
    item: object = None
    deadline: float = 0.0


def _stop_workers(workers: Iterable[_Worker], *, graceful: bool) -> None:
    rows = tuple(workers)
    if graceful:
        for worker in rows:
            if worker.process.is_alive():
                try:
                    worker.connection.send(None)
                except (BrokenPipeError, EOFError, OSError):
                    pass
    else:
        for worker in rows:
            if worker.process.is_alive():
                worker.process.terminate()
    for worker in rows:
        worker.connection.close()
    for worker in rows:
        worker.process.join(timeout=1.0)
        if worker.process.is_alive():
            worker.process.kill()
            worker.process.join()
        worker.process.close()


def supervised_ordered_map(
    function: Callable[[_Input], _Output],
    items: Iterable[_Input],
    *,
    workers: int,
    timeout_seconds: float,
    describe: Callable[[_Input], str],
) -> Iterator[_Output]:
    """Evaluate items concurrently, yielding input order with bounded buffering."""

    if workers < 1:
        raise ValueError("workers must be positive")
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise ValueError("worker timeout must be a finite positive number")

    context = multiprocessing.get_context()
    rows = []
    for _ in range(workers):
        parent, child = context.Pipe()
        process = context.Process(target=_worker_main, args=(child, function))
        try:
            process.start()
        except BaseException:
            parent.close()
            child.close()
            _stop_workers(rows, graceful=False)
            raise
        child.close()
        rows.append(_Worker(process=process, connection=parent))

    iterator = iter(enumerate(items))
    ready: Dict[int, _Output] = {}
    next_output = 0
    exhausted = False
    failed = True

    try:
        while True:
            while next_output in ready:
                value = ready.pop(next_output)
                next_output += 1
                yield value

            active = sum(worker.index is not None for worker in rows)
            for worker in rows:
                if (
                    exhausted
                    or worker.index is not None
                    or active + len(ready) >= workers * 2
                ):
                    continue
                try:
                    index, item = next(iterator)
                except StopIteration:
                    exhausted = True
                    break
                if not worker.process.is_alive():
                    worker.process.join(timeout=0)
                    raise RuntimeError(
                        f"worker exited with code {worker.process.exitcode}"
                        " before accepting work"
                    )
                worker.connection.send((index, item))
                worker.index = index
                worker.item = item
                worker.deadline = time.monotonic() + timeout_seconds
                active += 1

            active_rows = tuple(worker for worker in rows if worker.index is not None)
            if not active_rows:
                if exhausted and not ready:
                    break
                continue

            now = time.monotonic()
            timeout = max(
                0.0,
                min(worker.deadline for worker in active_rows) - now,
            )
            watched = [worker.connection for worker in active_rows]
            watched.extend(worker.process.sentinel for worker in rows)
            available = wait(watched, timeout=timeout)

            for worker in active_rows:
                if worker.connection not in available:
                    continue
                try:
                    message = worker.connection.recv()
                except EOFError as error:
                    worker.process.join(timeout=0.1)
                    exit_detail = (
                        f" with code {worker.process.exitcode}"
                        if worker.process.exitcode is not None
                        else ""
                    )
                    raise RuntimeError(
                        f"worker exited{exit_detail} before returning "
                        f"{describe(worker.item)} (input {worker.index + 1})"
                    ) from error
                if (
                    not isinstance(message, tuple)
                    or len(message) < 3
                    or message[1] != worker.index
                ):
                    raise RuntimeError("worker returned an invalid response")
                if message[0] == "error" and len(message) == 5:
                    raise RuntimeError(
                        f"worker {message[2]}.{message[3]} while processing "
                        f"{describe(worker.item)} (input {worker.index + 1}): "
                        f"{message[4]}"
                    )
                if message[0] != "result" or len(message) != 3:
                    raise RuntimeError("worker returned an invalid response")
                ready[worker.index] = message[2]
                worker.index = None
                worker.item = None
                worker.deadline = 0.0

            for worker in rows:
                if worker.process.sentinel not in available:
                    continue
                worker.process.join()
                context_label = (
                    f" while processing {describe(worker.item)}"
                    f" (input {worker.index + 1})"
                    if worker.index is not None
                    else ""
                )
                raise RuntimeError(
                    f"worker exited with code {worker.process.exitcode}"
                    f"{context_label}"
                )

            now = time.monotonic()
            expired = [
                worker
                for worker in rows
                if worker.index is not None and worker.deadline <= now
            ]
            if expired:
                worker = min(expired, key=lambda row: (row.deadline, row.index))
                raise TimeoutError(
                    f"worker did not complete within {timeout_seconds:g} seconds"
                    f" while processing {describe(worker.item)} "
                    f"(input {worker.index + 1})"
                )

        failed = False
    finally:
        _stop_workers(rows, graceful=not failed)
