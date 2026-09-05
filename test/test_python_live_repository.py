#!/usr/bin/env python3
"""Prove that the Python live host starts before initial Map analysis."""

from __future__ import annotations

from collections import OrderedDict
import hashlib
import io
from types import SimpleNamespace
import shutil
import sys
import threading
import time
from pathlib import Path

from archbird.serve import LiveRepository, create_live_server, _handler


def assert_display_retention(repository: Path) -> None:
    cache = LiveRepository(root=repository)
    cache.blob_byte_limit = 10
    snapshot = {"generation": "1" * 64, "project": "cache"}
    request = {"method": "map", "display": {}, "params": {}, "session": None}
    first = cache.artifact(snapshot, b"AAAAAA")
    cache.publish_display(request, first)
    second = cache.artifact(snapshot, b"BBBBBB")
    assert first["blob_sha256"] in cache.blobs
    cache.publish_display(request, second)
    assert list(cache.blobs) == [second["blob_sha256"]], list(cache.blobs)
    large = cache.artifact(snapshot, b"C" * 11)
    cache.publish_display(request, large)
    assert list(cache.blobs) == [large["blob_sha256"]]
    assert cache.blobs[large["blob_sha256"]] == b"C" * 11


def assert_disconnected_responses(app_root: Path) -> None:
    activities = []
    repository = SimpleNamespace(request=lambda _: {}, current=None,
                                 state=lambda: {}, _activity=lambda *args: activities.append(args))
    handler_type = _handler(repository, app_root)
    for method in ("POST", "GET", "HEAD"):
        for error_type in (BrokenPipeError, ConnectionResetError, None):
            handler = object.__new__(handler_type)
            body = b'{"method":"state"}'
            route = "/api/v1/request" if method == "POST" else "/api/v1/bootstrap"
            handler.rfile = io.BytesIO(f"{method} {route} HTTP/1.1\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)
            handler.wfile = io.BytesIO()
            if error_type:
                def disconnected(_data):
                    raise error_type("client disconnected")
                handler.wfile.write = disconnected
            handler._host_allowed = lambda: True
            handler.client_address = ("127.0.0.1", 1234)
            responses = []
            original = handler.send_response
            def response(status, message=None):
                responses.append(status)
                return original(status, message)
            handler.send_response = response
            handler.handle_one_request()
            assert responses == [200], responses
            if error_type:
                assert handler.close_connection
            else:
                assert b"200 OK" in handler.wfile.getvalue()
    assert not any(row[0] == "operation-failed" for row in activities)


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
    assert_display_retention(repository)
    assert_disconnected_responses(app_root)

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
        LiveRepository._candidate = original
        recovery = LiveRepository(root=repository)
        recovery.current = server.repository.current
        subscriber = recovery.subscribe()
        configuration_path = repository / "archbird.json"
        configuration = configuration_path.read_bytes()
        configuration_path.write_text('{"schema_version":2}\n', encoding="utf-8")
        recovery.rebuild()
        if recovery.state()["phase"] != "failed":
            raise AssertionError("invalid live candidate did not fail")
        configuration_path.write_bytes(configuration)
        recovery.rebuild()
        recovery_events = []
        while not subscriber.empty():
            recovery_events.append(subscriber.get_nowait())
        recovery.unsubscribe(subscriber)
        recovery.close()
        if not any(row and row.get("type") == "snapshot-ready" for row in recovery_events):
            raise AssertionError(
                "successful unchanged recovery did not announce the last good Map"
            )
        plan = {"id": "app-membership", "select": "component_membership"}
        first = server.repository.request(
            {"method": "projection", "payload": {"plan": plan}, "session": "test"}
        )
        second = server.repository.request(
            {
                "method": "projection",
                "payload": {
                    "plan": {
                        "select": "component_membership",
                        "id": "app-membership",
                    }
                },
                "session": "test",
            }
        )
        if first["blob_sha256"] != second["blob_sha256"]:
            raise AssertionError("equivalent projection requests changed artifact bytes")
        if len(server.repository.current["derived"]) != 1:
            raise AssertionError("equivalent projection plans were evaluated more than once")

        cache = LiveRepository(root=repository)
        cache.snapshot_byte_limit = 10
        cache.blob_byte_limit = 10
        first_snapshot = {
            "derived": OrderedDict(),
            "generation": "1" * 64,
            "map": b"123456",
            "project": "cache",
            "verification": b"",
        }
        second_snapshot = {
            "derived": OrderedDict(),
            "generation": "2" * 64,
            "map": b"abcdef",
            "project": "cache",
            "verification": b"",
        }
        cache.current = first_snapshot
        cache._retain_snapshot(first_snapshot)
        cache.current = second_snapshot
        cache._retain_snapshot(second_snapshot)
        if tuple(cache.snapshots) != (second_snapshot["generation"],):
            raise AssertionError("Python live snapshots exceeded their byte budget")
        if cache.derived(second_snapshot, "probe", {}, lambda: b"12345678") != b"12345678":
            raise AssertionError("uncached Python derived result changed")
        if second_snapshot["derived"]:
            raise AssertionError("oversize Python derived result was cached")
        first_blob = b"123456"
        second_blob = b"abcdef"
        cache.artifact(second_snapshot, first_blob)
        cache.artifact(second_snapshot, second_blob)
        if tuple(cache.blobs) != (hashlib.sha256(second_blob).hexdigest(),):
            raise AssertionError("Python live blobs exceeded their byte budget")

        poller = LiveRepository(root=repository, poll_seconds=0.01)
        poller.max_poll_seconds = 0.08
        signature_calls = 0

        def unchanged_signature():
            nonlocal signature_calls
            signature_calls += 1
            return ()

        poller._signature = unchanged_signature
        poller.start_watching()
        time.sleep(0.23)
        poller.close()
        if signature_calls > 7:
            raise AssertionError(
                f"idle Python watcher did not back off: {signature_calls} scans"
            )

        changing = LiveRepository(
            root=repository, poll_seconds=0.01, debounce_seconds=0.01
        )
        changing.max_poll_seconds = 0.08
        changed_value = 0
        changing_calls = 0
        rebuild_started = threading.Event()

        def changing_signature():
            nonlocal changing_calls
            changing_calls += 1
            return (("fixture", changed_value),)

        changing._signature = changing_signature
        changing.start_rebuild = rebuild_started.set
        changing.start_watching()
        time.sleep(0.12)
        changed_value = 1
        if not rebuild_started.wait(0.2):
            raise AssertionError("backed-off Python watcher missed a source change")
        calls_at_rebuild = changing_calls
        time.sleep(0.04)
        changing.close()
        if changing_calls - calls_at_rebuild < 2:
            raise AssertionError("Python watcher did not reset after a change")
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
