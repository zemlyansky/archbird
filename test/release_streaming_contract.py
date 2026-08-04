"""Shared installed-Python streaming contract used by release and mutation tests."""

from __future__ import annotations

from collections.abc import Callable
from typing import Protocol


class StreamingProject(Protocol):
    def map_json(self) -> bytes: ...

    def write_map_json(self, sink: Callable[[bytes], object]) -> None: ...


def validate_streaming_contract(
    project_factory: Callable[[], StreamingProject],
) -> bytes:
    project = project_factory()
    chunks: list[bytes] = []
    project.write_map_json(chunks.append)
    streamed = b"".join(chunks)
    if not streamed:
        raise AssertionError("installed Python streaming Map wrote no bytes")
    buffered = project.map_json()
    if streamed != buffered:
        raise AssertionError("installed Python buffered and streaming Maps differ")

    short_project = project_factory()
    try:
        short_project.write_map_json(
            lambda chunk: len(chunk) - 1 if chunk else 0
        )
    except OSError:
        pass
    else:
        raise AssertionError("installed Python streaming Map accepted a short write")

    class SinkError(RuntimeError):
        pass

    marker = SinkError("release-smoke-sink")

    def fail_sink(_chunk: bytes) -> None:
        raise marker

    failing_project = project_factory()
    try:
        failing_project.write_map_json(fail_sink)
    except SinkError as error:
        if error is not marker:
            raise AssertionError("installed Python streaming Map replaced sink error")
    else:
        raise AssertionError("installed Python streaming Map swallowed sink error")
    return buffered
