"""Loopback-only live repository host for the Archbird visualization app."""

from __future__ import annotations

import base64
from collections import OrderedDict
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
from pathlib import Path, PurePosixPath
import queue
import socket
import subprocess
import sys
import threading
import time
from typing import Any, Mapping, Optional, Sequence
from urllib.parse import unquote_to_bytes, urlsplit

from .native import diff_maps_json, export_graph, query_map_json


PROTOCOL_VERSION = 1
SNAPSHOT_LIMIT = 4
BLOB_LIMIT = 32
BODY_LIMIT = 2 * 1024 * 1024
TEXT_LIMIT = 256 * 1024
HEX_LIMIT = 64 * 1024
CAPABILITIES = ["map", "view", "query", "diff", "source", "snapshots", "events"]
MIME = {
    ".css": "text/css; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".wasm": "application/wasm",
}


def _canonical(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def _app_root(explicit: Optional[Path | str]) -> Path:
    package = Path(__file__).resolve().parent
    candidates = [
        Path(explicit).resolve() if explicit is not None else None,
        package / "app",
        package.parents[1] / "app" / "dist",
    ]
    for candidate in candidates:
        if candidate is not None and (candidate / "index.html").is_file():
            return candidate
    raise RuntimeError(
        "visualization application is unavailable; build app/dist or install package app assets"
    )


def _safe_relative(root: Path, value: object) -> tuple[Path, str]:
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError("source path must be a non-empty repository-relative string")
    logical = PurePosixPath(value.replace("\\", "/"))
    if logical.is_absolute() or ".." in logical.parts:
        raise ValueError(f"source path escapes repository root: {value}")
    candidate = root.joinpath(*logical.parts).resolve()
    try:
        relative = candidate.relative_to(root).as_posix()
    except ValueError as error:
        raise ValueError(f"source path escapes repository root: {value}") from error
    if not relative:
        raise ValueError("source path must select a file")
    return candidate, relative


def _visible_source(data: bytes) -> tuple[str, str, bool]:
    try:
        data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        visible = data[:HEX_LIMIT]
        return "hex", visible.hex(), len(data) > len(visible)
    visible = data[:TEXT_LIMIT]
    while visible:
        try:
            text = visible.decode("utf-8", errors="strict")
            return "utf-8", text, len(data) > len(visible)
        except UnicodeDecodeError as error:
            if len(data) == len(visible) or error.reason != "unexpected end of data":
                raise
            visible = visible[:-1]
    return "utf-8", "", bool(data)


class LiveRepository:
    """Maintain content-addressed last-good Maps and bounded source evidence."""

    def __init__(
        self,
        root: Path | str,
        *,
        config: Optional[Path | str] = None,
        config_json: Optional[bytes] = None,
        no_config: bool = False,
        project_options: Optional[Mapping[str, object]] = None,
        poll_seconds: float = 0.35,
        debounce_seconds: float = 0.18,
    ) -> None:
        self.root = Path(root).resolve()
        self.config = Path(config).resolve() if config is not None else None
        self.config_json = bytes(config_json) if config_json is not None else None
        self.no_config = bool(no_config)
        self.project_options = dict(project_options or {})
        self.poll_seconds = poll_seconds
        self.debounce_seconds = debounce_seconds
        self.current: Optional[dict[str, Any]] = None
        self.snapshots: "OrderedDict[str, dict[str, Any]]" = OrderedDict()
        self.blobs: "OrderedDict[str, bytes]" = OrderedDict()
        self._selections: dict[str, str] = {}
        self._subscribers: set[queue.Queue[Optional[dict[str, object]]]] = set()
        self._event_id = 0
        self._watch_paths: tuple[Path, ...] = (self.root,)
        self._watch_state: tuple[tuple[str, object], ...] = ()
        self._lock = threading.RLock()
        self._build_lock = threading.Lock()
        self._closed = threading.Event()
        self._watcher: Optional[threading.Thread] = None
        self._builder: Optional[threading.Thread] = None
        self._rebuild_pending = False
        self._phase = "waiting"
        self._last_error: Optional[str] = None

    def state(self) -> dict[str, object]:
        with self._lock:
            snapshot = self.current
            return {
                "generation": snapshot["generation"] if snapshot else None,
                "last_error": self._last_error,
                "phase": self._phase,
                "project": snapshot["project"] if snapshot else None,
                "source_available": snapshot is not None,
            }

    def _set_phase(self, phase: str, error: Optional[str] = None) -> None:
        with self._lock:
            self._phase = phase
            self._last_error = error

    def _event(
        self, event_type: str, payload: Mapping[str, object], generation: Optional[str] = None
    ) -> None:
        with self._lock:
            event: dict[str, object] = {
                "event_id": self._event_id,
                "payload": dict(payload),
                "protocol_version": PROTOCOL_VERSION,
                "session": "server",
                "type": event_type,
            }
            self._event_id += 1
            if generation:
                event["generation"] = generation
            for subscriber in tuple(self._subscribers):
                try:
                    subscriber.put_nowait(event)
                except queue.Full:
                    self._subscribers.discard(subscriber)

    def subscribe(self) -> queue.Queue[Optional[dict[str, object]]]:
        result: queue.Queue[Optional[dict[str, object]]] = queue.Queue(maxsize=64)
        with self._lock:
            self._subscribers.add(result)
        return result

    def unsubscribe(self, subscriber: queue.Queue[Optional[dict[str, object]]]) -> None:
        with self._lock:
            self._subscribers.discard(subscriber)

    def _candidate(self) -> dict[str, Any]:
        request = {
            "config_base64": (
                base64.b64encode(self.config_json).decode("ascii")
                if self.config_json is not None
                else None
            ),
            "config_path": str(self.config) if self.config is not None else None,
            "no_config": self.no_config,
            "project_options": self.project_options,
            "root": str(self.root),
        }
        completed = subprocess.run(
            [sys.executable, "-m", "archbird._serve_worker"],
            input=_canonical(request),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode:
            message = completed.stderr.decode("utf-8", errors="replace").strip()
            for prefix in (str(self.root) + "/", str(self.config.parent) + "/" if self.config else ""):
                if prefix:
                    message = message.replace(prefix, "")
            raise RuntimeError(message.rsplit("\n", 1)[-1] or "Map candidate failed")
        header_bytes, separator, map_json = completed.stdout.partition(b"\n")
        if not separator:
            raise RuntimeError("Map candidate returned no framed artifact")
        header = json.loads(header_bytes)
        if len(map_json) != header.get("map_bytes"):
            raise RuntimeError("Map candidate artifact size changed in transport")
        generation = header.get("generation")
        if not isinstance(generation, str) or len(generation) != 64:
            raise RuntimeError("Map candidate generation is invalid")
        return {
            "files": list(header["files"]),
            "generation": generation,
            "map": map_json,
            "project": header["project"],
            "stored_at": int(time.time() * 1000),
        }

    def _sync_watch_paths(self, files: Sequence[Mapping[str, object]]) -> None:
        paths = {self.root}
        if self.config is not None:
            paths.add(self.config)
            paths.add(self.config.parent)
        for row in files:
            candidate = self.root.joinpath(*str(row["path"]).split("/"))
            paths.add(candidate)
            parent = candidate.parent
            while parent == self.root or self.root in parent.parents:
                paths.add(parent)
                if parent == self.root:
                    break
                parent = parent.parent
        self._watch_paths = tuple(sorted(paths, key=lambda path: str(path).encode()))

    def _signature(self) -> tuple[tuple[str, object], ...]:
        rows = []
        for path in self._watch_paths:
            try:
                stat = path.lstat()
                value: object = (
                    stat.st_mode,
                    stat.st_ino,
                    stat.st_size,
                    stat.st_mtime_ns,
                )
            except FileNotFoundError:
                value = None
            rows.append((str(path), value))
        return tuple(rows)

    def rebuild(self) -> None:
        if self._closed.is_set():
            return
        with self._build_lock:
            current_generation = self.current["generation"] if self.current else None
            self._set_phase("analyzing")
            self._event(
                "scan-started",
                {"phase": "analyzing", "root": "."},
                current_generation,
            )
            try:
                candidate = self._candidate()
            except Exception as error:
                message = str(error)
                self._set_phase("failed", message)
                self._event(
                    "candidate-failed",
                    {"message": message, "phase": "failed"},
                    current_generation,
                )
                if self.current is None:
                    raise
                return
            with self._lock:
                changed = self.current is None or candidate["generation"] != current_generation
                self.current = candidate
                self.snapshots.pop(candidate["generation"], None)
                self.snapshots[candidate["generation"]] = candidate
                while len(self.snapshots) > SNAPSHOT_LIMIT:
                    self.snapshots.popitem(last=False)
                self._sync_watch_paths(candidate["files"])
                self._watch_state = self._signature()
                self._phase = "ready"
                self._last_error = None
            if changed:
                self._event(
                    "snapshot-ready",
                    {
                        "files": len(candidate["files"]),
                        "phase": "ready",
                        "project": candidate["project"],
                    },
                    candidate["generation"],
                )

    def start_rebuild(self) -> None:
        if self._closed.is_set():
            return
        with self._lock:
            if self._builder is not None and self._builder.is_alive():
                self._rebuild_pending = True
                return
            self._rebuild_pending = False

            def build() -> None:
                while not self._closed.is_set():
                    try:
                        self.rebuild()
                    except Exception:
                        pass
                    with self._lock:
                        if self._rebuild_pending and not self._closed.is_set():
                            self._rebuild_pending = False
                            continue
                        self._builder = None
                        break

            self._builder = threading.Thread(
                target=build, name="archbird-repository-build", daemon=True
            )
            self._builder.start()

    def start_watching(self) -> None:
        if self._watcher is not None:
            return
        self._watch_state = self._signature()

        def watch() -> None:
            while not self._closed.wait(self.poll_seconds):
                observed = self._signature()
                if observed == self._watch_state:
                    continue
                self._set_phase("stale")
                self._event(
                    "progress",
                    {"phase": "stale", "root": "."},
                    self.current["generation"] if self.current else None,
                )
                if self._closed.wait(self.debounce_seconds):
                    break
                self._watch_state = self._signature()
                self.start_rebuild()

        self._watcher = threading.Thread(
            target=watch, name="archbird-repository-watch", daemon=True
        )
        self._watcher.start()

    def start(self) -> None:
        self.start_watching()
        self.start_rebuild()

    def snapshot(self, generation: Optional[str] = None) -> dict[str, Any]:
        with self._lock:
            selected = self.snapshots.get(generation) if generation else self.current
            if selected is None:
                raise ValueError(f"snapshot is unavailable: {generation or 'current'}")
            return selected

    def artifact(self, snapshot: Mapping[str, Any], data: bytes) -> dict[str, object]:
        digest = hashlib.sha256(data).hexdigest()
        with self._lock:
            self.blobs.pop(digest, None)
            self.blobs[digest] = bytes(data)
            while len(self.blobs) > BLOB_LIMIT:
                self.blobs.popitem(last=False)
        return {
            "blob_sha256": digest,
            "bytes": len(data),
            "generation": snapshot["generation"],
            "project": snapshot["project"],
        }

    def source(self, snapshot: Mapping[str, Any], source_path: object) -> dict[str, object]:
        allowed = {str(row["path"]): row for row in snapshot["files"]}
        candidate, relative = _safe_relative(self.root, source_path)
        evidence = allowed.get(relative)
        if evidence is None:
            raise ValueError(f"source is not mapped in this generation: {relative}")
        metadata = candidate.lstat()
        if not candidate.is_file() or candidate.is_symlink():
            raise ValueError(f"source is not a regular current file: {relative}")
        data = candidate.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != evidence["sha256"]:
            raise ValueError(f"source changed after selected generation: {relative}")
        encoding, text, truncated = _visible_source(data)
        return {
            "bytes": metadata.st_size,
            "encoding": encoding,
            "path": relative,
            "sha256": digest,
            "text": text,
            "truncated": truncated,
        }

    def request(self, request: Mapping[str, Any]) -> object:
        method = request["method"]
        payload = request["payload"]
        session = request["session"]
        if method == "bootstrap":
            return {"capabilities": CAPABILITIES, "engine": "native"}
        if method == "state":
            return self.state()
        if method == "snapshots":
            return [
                {
                    "files": len(row["files"]),
                    "generation": row["generation"],
                    "project": row["project"],
                    "stored_at": row["stored_at"],
                }
                for row in reversed(tuple(self.snapshots.values()))
            ]
        if method == "open-snapshot":
            snapshot = self.snapshot(payload.get("generation"))
            self._selections[session] = snapshot["generation"]
            return self.artifact(snapshot, snapshot["map"])
        generation = payload.get("generation") or self._selections.get(session)
        snapshot = self.snapshot(generation)
        if method == "map":
            return self.artifact(snapshot, snapshot["map"])
        if method == "view":
            data = export_graph(
                snapshot["map"],
                format="json",
                view=str(payload.get("view") or "components"),
                max_nodes=int(payload.get("max_nodes", 0)),
                max_edge_names=int(payload.get("max_edge_names", 3)),
            )
            return self.artifact(snapshot, data)
        if method == "query":
            raw = payload.get("query") or {}
            allowed = {
                "focus", "paths", "symbols", "components", "packages", "artifacts",
                "direction", "depth", "test_depth",
            }
            query = {key: value for key, value in raw.items() if key in allowed}
            return self.artifact(snapshot, query_map_json(snapshot["map"], **query))
        if method == "diff":
            before = self.snapshot(payload.get("before"))
            after = self.snapshot(payload.get("after"))
            return self.artifact(after, diff_maps_json(before["map"], after["map"]))
        if method == "source":
            return self.source(snapshot, payload.get("path"))
        if method in {"verification", "act-proposal", "act-contract", "act-result"}:
            return None
        if method == "dispose":
            self._selections.pop(session, None)
            return {"disposed": True}
        raise ValueError(f"unsupported visualization method: {method}")

    def close(self) -> None:
        self._closed.set()
        if self._watcher is not None:
            self._watcher.join(timeout=5)
        if self._builder is not None:
            self._builder.join(timeout=30)
        with self._lock:
            for subscriber in self._subscribers:
                try:
                    subscriber.put_nowait(None)
                except queue.Full:
                    pass
            self._subscribers.clear()


def _validate_request(value: object) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError("request must be an object")
    if value.get("protocol_version") != PROTOCOL_VERSION:
        raise ValueError("unsupported visualization protocol")
    for name in ("id", "method", "session"):
        if not isinstance(value.get(name), str) or not value[name]:
            raise ValueError(f"request.{name} is required")
    if not isinstance(value.get("payload"), dict):
        raise ValueError("request.payload must be an object")
    return value


def _handler(repository: LiveRepository, static_root: Path):
    class Handler(BaseHTTPRequestHandler):
        server_version = "Archbird/1"

        def log_message(self, format: str, *args: object) -> None:
            return

        def _headers(self) -> None:
            self.send_header(
                "Content-Security-Policy",
                "default-src 'self'; connect-src 'self'; img-src 'self' data:; "
                "object-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                "worker-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'",
            )
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")

        def _host_allowed(self) -> bool:
            host = self.headers.get("Host", "")
            allowed = {
                f"127.0.0.1:{self.server.server_port}",
                f"localhost:{self.server.server_port}",
                f"[::1]:{self.server.server_port}",
            }
            if host not in allowed:
                self.send_error(403, "forbidden host")
                return False
            origin = self.headers.get("Origin")
            if origin and origin != f"http://{host}":
                self.send_error(403, "forbidden origin")
                return False
            return True

        def _json(self, status: int, value: object) -> None:
            data = _canonical(value)
            self.send_response(status)
            self._headers()
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def _path(self) -> str:
            encoded = unquote_to_bytes(urlsplit(self.path).path)
            return encoded.decode("utf-8", errors="strict")

        def do_GET(self) -> None:
            self._get_or_head()

        def do_HEAD(self) -> None:
            self._get_or_head()

        def _get_or_head(self) -> None:
            if not self._host_allowed():
                return
            try:
                pathname = self._path()
            except (UnicodeDecodeError, ValueError):
                self.send_error(400, "invalid request path")
                return
            if pathname == "/api/v1/bootstrap":
                self._json(200, {
                    "capabilities": CAPABILITIES,
                    "protocol_version": PROTOCOL_VERSION,
                    **repository.state(),
                })
                return
            if pathname == "/api/v1/events":
                self.send_response(200)
                self._headers()
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "keep-alive")
                self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                self.end_headers()
                self.wfile.write(b": archbird live events\n\n")
                self.wfile.flush()
                subscriber = repository.subscribe()
                try:
                    while True:
                        try:
                            event = subscriber.get(timeout=15)
                        except queue.Empty:
                            self.wfile.write(b": keepalive\n\n")
                            self.wfile.flush()
                            continue
                        if event is None:
                            break
                        data = _canonical(event)
                        self.wfile.write(
                            f"id: {event['event_id']}\nevent: {event['type']}\ndata: ".encode()
                            + data
                            + b"\n\n"
                        )
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    repository.unsubscribe(subscriber)
                return
            prefix = "/api/v1/blobs/"
            if pathname.startswith(prefix):
                digest = pathname[len(prefix):]
                data = repository.blobs.get(digest) if len(digest) == 64 else None
                if data is None:
                    self.send_error(404, "unknown artifact blob")
                    return
                self.send_response(200)
                self._headers()
                self.send_header("Cache-Control", "private, max-age=31536000, immutable")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("X-Content-SHA256", digest)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(data)
                return
            relative = "index.html" if pathname == "/" else pathname.lstrip("/")
            candidate = (static_root / relative).resolve()
            try:
                candidate.relative_to(static_root)
            except ValueError:
                self.send_error(403, "forbidden")
                return
            if not candidate.is_file() or candidate.is_symlink():
                if Path(relative).suffix:
                    self.send_error(404, "not found")
                    return
                candidate = static_root / "index.html"
            data = candidate.read_bytes()
            self.send_response(200)
            self._headers()
            self.send_header(
                "Cache-Control",
                "no-cache" if candidate.name == "index.html" else "public, max-age=31536000, immutable",
            )
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Type", MIME.get(candidate.suffix, mimetypes.guess_type(candidate)[0] or "application/octet-stream"))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def do_POST(self) -> None:
            if not self._host_allowed():
                return
            try:
                if self._path() != "/api/v1/request":
                    self.send_error(404, "not found")
                    return
                raw_length = self.headers.get("Content-Length")
                if raw_length is None or not raw_length.isdigit():
                    raise ValueError("request Content-Length is required")
                length = int(raw_length)
                if length > BODY_LIMIT:
                    raise ValueError(f"request body exceeds {BODY_LIMIT} bytes")
                raw: object = json.loads(self.rfile.read(length))
                request = _validate_request(raw)
                result = repository.request(request)
                envelope = {
                    "generation": repository.current["generation"] if repository.current else None,
                    "id": request["id"],
                    "ok": True,
                    "protocol_version": PROTOCOL_VERSION,
                    "result": result,
                    "session": request["session"],
                }
                self._json(200, envelope)
            except Exception as error:
                identity = locals().get("raw") if isinstance(locals().get("raw"), dict) else {}
                self._json(400, {
                    "error": {"code": "host-error", "message": str(error)},
                    "id": identity.get("id", "invalid"),
                    "ok": False,
                    "protocol_version": PROTOCOL_VERSION,
                    "session": identity.get("session", "invalid"),
                })

    return Handler


class LiveServer:
    def __init__(self, repository: LiveRepository, server: ThreadingHTTPServer) -> None:
        self.repository = repository
        self.server = server
        host, port = server.server_address[:2]
        self.url = f"http://{'[::1]' if host == '::1' else host}:{port}/"
        self._thread = threading.Thread(
            target=server.serve_forever, name="archbird-http", daemon=True
        )
        self._thread.start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.repository.close()
        self._thread.join(timeout=5)

    def wait(self) -> None:
        self._thread.join()


def create_live_server(
    *,
    root: Path | str = ".",
    config: Optional[Path | str] = None,
    config_json: Optional[bytes] = None,
    no_config: bool = False,
    project_options: Optional[Mapping[str, object]] = None,
    app: Optional[Path | str] = None,
    host: str = "127.0.0.1",
    port: int = 4177,
) -> LiveServer:
    if host not in {"127.0.0.1", "::1"}:
        raise ValueError("live server binds only to 127.0.0.1 or ::1")
    repository = LiveRepository(
        root,
        config=config,
        config_json=config_json,
        no_config=no_config,
        project_options=project_options,
    )
    static_root = _app_root(app)
    family = socket.AF_INET6 if host == "::1" else socket.AF_INET
    server_class = type(
        "ArchbirdHTTPServer",
        (ThreadingHTTPServer,),
        {"address_family": family, "daemon_threads": True},
    )
    server = server_class((host, port), _handler(repository, static_root))
    live = LiveServer(repository, server)
    repository.start()
    return live


__all__ = ["LiveRepository", "LiveServer", "create_live_server"]
