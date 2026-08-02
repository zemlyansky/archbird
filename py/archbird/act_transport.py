"""Filesystem transport for native Plan materialization and accepted Acts."""

from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import signal
import stat
import subprocess
import tempfile
import threading
import time
from types import MappingProxyType
from typing import Mapping

from . import _native


_MAX_FILE_BYTES = 64 * 1024 * 1024
_GATE_DEFAULT_OUTPUT_BYTES = 1024 * 1024
_GATE_TAIL_BYTES = 64 * 1024


def _relative_path(value: object) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError("repository path must be non-empty text")
    posix = PurePosixPath(value)
    windows = PureWindowsPath(value)
    if (
        posix.is_absolute()
        or windows.is_absolute()
        or windows.drive
        or "\\" in value
        or any(part in {"", ".", ".."} for part in posix.parts)
        or posix.as_posix() != value
    ):
        raise ValueError(f"unsafe repository path: {value}")
    return value


def _candidate(root: Path, relative: str) -> Path:
    return root.joinpath(*PurePosixPath(relative).parts)


def _repository_root(root: Path) -> Path:
    resolved = root.resolve(strict=True)
    if not resolved.is_dir():
        raise OSError("repository root must resolve to a directory")
    return resolved


def _check_parents(root: Path, relative: str) -> None:
    cursor = root
    for part in PurePosixPath(relative).parts[:-1]:
        cursor /= part
        try:
            metadata = cursor.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            raise OSError(f"unsafe repository path parent: {relative}")


def _read_regular(root: Path, relative: str) -> tuple[bytes, int]:
    _check_parents(root, relative)
    path = _candidate(root, relative)
    before = path.lstat()
    if stat.S_ISLNK(before.st_mode) or not stat.S_ISREG(before.st_mode):
        raise OSError(f"source is not a regular file: {relative}")
    if before.st_size > _MAX_FILE_BYTES:
        raise OSError(
            f"source exceeds the {_MAX_FILE_BYTES}-byte Act limit: {relative}"
        )
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or opened.st_dev != before.st_dev
            or opened.st_ino != before.st_ino
        ):
            raise OSError(f"source changed while opening: {relative}")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(1024 * 1024, _MAX_FILE_BYTES + 1))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > _MAX_FILE_BYTES:
                raise OSError(
                    f"source exceeds the {_MAX_FILE_BYTES}-byte Act limit: "
                    f"{relative}"
                )
        after = os.fstat(descriptor)
        if (
            after.st_dev != opened.st_dev
            or after.st_ino != opened.st_ino
            or after.st_size != opened.st_size
            or after.st_mtime_ns != opened.st_mtime_ns
        ):
            raise OSError(f"source changed while reading: {relative}")
        return b"".join(chunks), opened.st_mode
    finally:
        os.close(descriptor)


def _require_absent(root: Path, relative: str) -> None:
    _check_parents(root, relative)
    try:
        _candidate(root, relative).lstat()
    except FileNotFoundError:
        return
    raise OSError(f"Act destination already exists: {relative}")


def observe_source_requirements(
    root: Path, requirements_json: bytes
) -> bytes:
    root = _repository_root(root)
    document = json.loads(requirements_json)
    if not isinstance(document, dict) or set(document) != {
        "files",
        "absent",
        "observe",
    }:
        raise ValueError("native source requirements have an invalid shape")
    files = document["files"]
    absent = document["absent"]
    observe = document["observe"]
    if (
        not isinstance(files, list)
        or not isinstance(absent, list)
        or not isinstance(observe, list)
    ):
        raise ValueError("native source requirements must contain arrays")
    rows: list[dict[str, object]] = []
    for raw_path in files:
        path = _relative_path(raw_path)
        data, mode = _read_regular(root, path)
        rows.append(
            {
                "path": path,
                "sha256": hashlib.sha256(data).hexdigest(),
                "executable": bool(mode & 0o111),
            }
        )
    absent_paths: list[str] = []
    for raw_path in absent:
        path = _relative_path(raw_path)
        _require_absent(root, path)
        absent_paths.append(path)
    for raw_path in observe:
        path = _relative_path(raw_path)
        try:
            data, mode = _read_regular(root, path)
        except FileNotFoundError:
            absent_paths.append(path)
            continue
        rows.append(
            {
                "path": path,
                "sha256": hashlib.sha256(data).hexdigest(),
                "executable": bool(mode & 0o111),
            }
        )
    metadata = {
        "files": sorted(rows, key=lambda row: str(row["path"])),
        "absent": sorted(absent_paths),
    }
    encoded = json.dumps(
        metadata,
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return _native.json_canonicalize(encoded)


def observe_plan_sources(
    root: Path,
    plan_json: bytes,
    executor_submissions_json: bytes = b"",
) -> bytes:
    requirements = _native.plan_source_requirements(
        plan_json, executor_submissions_json
    )
    return observe_source_requirements(root, requirements)


def observe_act_sources(root: Path, act_json: bytes) -> bytes:
    requirements = _native.act_source_requirements(act_json)
    root = _repository_root(root)
    document = json.loads(requirements)
    if not isinstance(document, dict) or set(document) != {"paths"}:
        raise ValueError("native Act source requirements have an invalid shape")
    paths = document["paths"]
    if not isinstance(paths, list):
        raise ValueError("native Act source requirements must contain paths")
    rows: list[dict[str, object]] = []
    absent: list[str] = []
    for raw_path in paths:
        path = _relative_path(raw_path)
        try:
            data, mode = _read_regular(root, path)
        except FileNotFoundError:
            absent.append(path)
            continue
        rows.append(
            {
                "path": path,
                "sha256": hashlib.sha256(data).hexdigest(),
                "executable": bool(mode & 0o111),
            }
        )
    metadata = {
        "files": sorted(rows, key=lambda row: str(row["path"])),
        "absent": sorted(absent),
    }
    return _native.json_canonicalize(
        json.dumps(
            metadata,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    )


def act_overlay(act_json: bytes) -> Mapping[str, bytes | None]:
    _native.act_validate(act_json)
    document = json.loads(act_json)
    transitions = document.get("transitions")
    if not isinstance(transitions, list):
        raise ValueError("Act has no transitions")
    overlay: dict[str, bytes | None] = {}
    for transition in transitions:
        if not isinstance(transition, dict):
            raise ValueError("Act transition must be an object")
        kind = transition.get("kind")
        path = _relative_path(transition.get("path"))
        after = transition.get("after")
        if kind == "delete":
            overlay[path] = None
            continue
        if not isinstance(after, dict):
            raise ValueError("Act transition has no after-state")
        content = after.get("content_base64")
        if not isinstance(content, str):
            raise ValueError("Act after-state has no content")
        try:
            data = base64.b64decode(content, validate=True)
        except (ValueError, TypeError) as error:
            raise ValueError("Act after-state has invalid base64") from error
        if kind == "move":
            source = _relative_path(transition.get("source_path"))
            overlay[source] = None
        elif kind not in {"create", "modify"}:
            raise ValueError(f"unsupported Act transition kind: {kind}")
        overlay[path] = data
    return MappingProxyType(dict(sorted(overlay.items())))


def _safe_symlink(root: Path, relative: str) -> str:
    path = _candidate(root, relative)
    target = os.readlink(path)
    if os.path.isabs(target):
        raise OSError(f"absolute symbolic link is not isolated: {relative}")
    resolved = (path.parent / target).resolve(strict=False)
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise OSError(
            f"symbolic link escapes the repository: {relative}"
        ) from error
    return target


def _recursive_inventory(root: Path, prefix: str = "") -> list[str]:
    start = _candidate(root, prefix) if prefix else root
    rows: list[str] = []
    for directory, names, files in os.walk(start, followlinks=False):
        directory_path = Path(directory)
        relative_directory = directory_path.relative_to(root).as_posix()
        names[:] = sorted(
            name
            for name in names
            if name != ".git" and not name.startswith(".archbird-gates-")
        )
        for name in sorted(files):
            relative = (
                f"{relative_directory}/{name}"
                if relative_directory != "."
                else name
            )
            if relative == ".archbird-apply.lock":
                continue
            rows.append(_relative_path(relative))
        for name in list(names):
            relative = (
                f"{relative_directory}/{name}"
                if relative_directory != "."
                else name
            )
            metadata = _candidate(root, relative).lstat()
            if stat.S_ISLNK(metadata.st_mode):
                rows.append(_relative_path(relative))
                names.remove(name)
    return rows


def _repository_inventory(root: Path) -> list[str]:
    try:
        top = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
        ).stdout.decode("utf-8", "strict").strip()
        if Path(top).resolve(strict=True) != root:
            return _recursive_inventory(root)
        output = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "ls-files",
                "-z",
                "--cached",
                "--others",
                "--exclude-standard",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        ).stdout
        rows: list[str] = []
        for raw in output.split(b"\0"):
            if not raw:
                continue
            relative = _relative_path(raw.decode("utf-8", "strict"))
            path = _candidate(root, relative)
            try:
                metadata = path.lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISDIR(metadata.st_mode):
                rows.extend(_recursive_inventory(root, relative))
            else:
                rows.append(relative)
        return sorted(set(rows))
    except (FileNotFoundError, subprocess.SubprocessError, UnicodeError):
        return _recursive_inventory(root)


def _copy_gate_workspace(root: Path, workspace: Path) -> None:
    for relative in _repository_inventory(root):
        source = _candidate(root, relative)
        destination = _candidate(workspace, relative)
        metadata = source.lstat()
        destination.parent.mkdir(parents=True, exist_ok=True)
        if stat.S_ISLNK(metadata.st_mode):
            destination.symlink_to(_safe_symlink(root, relative))
        elif stat.S_ISREG(metadata.st_mode):
            shutil.copy2(source, destination, follow_symlinks=False)
        else:
            raise OSError(
                f"unsupported repository entry in gate workspace: {relative}"
            )


def _write_workspace_file(path: Path, data: bytes, executable: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    mode = path.stat().st_mode
    mode = mode | 0o111 if executable else mode & ~0o111
    path.chmod(stat.S_IMODE(mode))


def _apply_materialized_act(workspace: Path, act_json: bytes) -> None:
    document = json.loads(act_json)
    for transition in document["transitions"]:
        kind = transition["kind"]
        relative = _relative_path(transition["path"])
        destination = _candidate(workspace, relative)
        source_relative = (
            _relative_path(transition["source_path"])
            if kind == "move"
            else relative
        )
        source = _candidate(workspace, source_relative)
        before = transition["before"]
        if kind == "create":
            if destination.exists() or destination.is_symlink():
                raise OSError(f"gate workspace destination exists: {relative}")
        else:
            data, mode = _read_regular(workspace, source_relative)
            if (
                hashlib.sha256(data).hexdigest() != before["sha256"]
                or bool(mode & 0o111) != before["executable"]
            ):
                raise OSError(
                    f"gate workspace source differs from Act: {source_relative}"
                )
        if kind == "delete":
            source.unlink()
            continue
        if kind == "move" and (destination.exists() or destination.is_symlink()):
            raise OSError(f"gate workspace destination exists: {relative}")
        after = transition["after"]
        data = base64.b64decode(after["content_base64"], validate=True)
        _check_parents(workspace, relative)
        _write_workspace_file(destination, data, after["executable"])
        if kind == "move":
            source.unlink()


def _workspace_sha256(workspace: Path) -> str:
    digest = hashlib.sha256()
    for relative in _recursive_inventory(workspace):
        path = _candidate(workspace, relative)
        metadata = path.lstat()
        encoded_path = relative.encode("utf-8")
        if stat.S_ISLNK(metadata.st_mode):
            target = os.readlink(path).encode("utf-8")
            digest.update(b"L\0")
            digest.update(encoded_path)
            digest.update(b"\0")
            digest.update(target)
            digest.update(b"\0")
        elif stat.S_ISREG(metadata.st_mode):
            data = path.read_bytes()
            digest.update(b"F\0")
            digest.update(encoded_path)
            digest.update(b"\0")
            digest.update(b"1\0" if metadata.st_mode & 0o111 else b"0\0")
            digest.update(hashlib.sha256(data).digest())
        else:
            raise OSError(f"unsupported gate workspace entry: {relative}")
    return digest.hexdigest()


def _gate_environment(cwd: Path) -> dict[str, str]:
    environment = dict(os.environ)
    environment["PWD"] = str(cwd)
    return environment


def _environment_sha256(environment: Mapping[str, str]) -> str:
    encoded = json.dumps(
        dict(sorted(environment.items())),
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(_native.json_canonicalize(encoded)).hexdigest()


class _GateOutput:
    def __init__(self, limit: int) -> None:
        self.limit = limit
        self.total = 0
        self.exceeded = False
        self.lock = threading.Lock()

    def account(self, length: int) -> None:
        with self.lock:
            self.total += length
            if self.total > self.limit:
                self.exceeded = True


class _GateStream:
    def __init__(self, output: _GateOutput) -> None:
        self.output = output
        self.digest = hashlib.sha256()
        self.tail = bytearray()

    def read(self, stream: object) -> None:
        while True:
            chunk = stream.read(64 * 1024)
            if not chunk:
                return
            self.digest.update(chunk)
            self.output.account(len(chunk))
            self.tail.extend(chunk)
            if len(self.tail) > _GATE_TAIL_BYTES:
                del self.tail[: len(self.tail) - _GATE_TAIL_BYTES]


def _terminate_gate(process: subprocess.Popen[bytes]) -> None:
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
    except (OSError, ProcessLookupError):
        pass


def _gate_result(
    gate_id: str,
    definition: Mapping[str, object],
    workspace: Path,
) -> dict[str, object]:
    canonical_definition = _native.json_canonicalize(
        json.dumps(
            definition,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    )
    definition_sha256 = hashlib.sha256(canonical_definition).hexdigest()
    argv = definition["argv"]
    cwd = _candidate(workspace, str(definition.get("cwd", ".")))
    environment = _gate_environment(cwd)
    environment_sha256 = _environment_sha256(environment)
    output = _GateOutput(
        int(definition.get("max_output_bytes", _GATE_DEFAULT_OUTPUT_BYTES))
    )
    stdout = _GateStream(output)
    stderr = _GateStream(output)
    started = time.monotonic()
    status = "error"
    exit_code: int | None = None
    process: subprocess.Popen[bytes] | None = None
    try:
        process = subprocess.Popen(
            argv,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            start_new_session=os.name == "posix",
        )
        stdout_thread = threading.Thread(
            target=stdout.read, args=(process.stdout,), daemon=True
        )
        stderr_thread = threading.Thread(
            target=stderr.read, args=(process.stderr,), daemon=True
        )
        stdout_thread.start()
        stderr_thread.start()
        deadline = started + int(definition["timeout_seconds"])
        while process.poll() is None:
            if output.exceeded:
                status = "output_limit"
                _terminate_gate(process)
                break
            if time.monotonic() >= deadline:
                status = "timeout"
                _terminate_gate(process)
                break
            time.sleep(0.01)
        process.wait()
        stdout_thread.join()
        stderr_thread.join()
        if output.exceeded:
            status = "output_limit"
            exit_code = None
        elif status not in {"timeout", "output_limit"}:
            if process.returncode == 0:
                status = "pass"
                exit_code = 0
            else:
                status = "fail"
                exit_code = (
                    process.returncode if process.returncode >= 0 else None
                )
    except (OSError, ValueError):
        if process is not None and process.poll() is None:
            _terminate_gate(process)
            process.wait()
    duration_ms = max(0, int((time.monotonic() - started) * 1000))
    return {
        "id": gate_id,
        "definition_sha256": definition_sha256,
        "status": status,
        "exit_code": exit_code,
        "duration_ms": duration_ms,
        "environment_sha256": environment_sha256,
        "stdout_sha256": stdout.digest.hexdigest(),
        "stderr_sha256": stderr.digest.hexdigest(),
        "stdout_tail_base64": base64.b64encode(stdout.tail).decode("ascii"),
        "stderr_tail_base64": base64.b64encode(stderr.tail).decode("ascii"),
    }


def _gate_order(gates: Mapping[str, Mapping[str, object]]) -> list[str]:
    remaining = set(gates)
    emitted: list[str] = []
    while remaining:
        ready = sorted(
            gate_id
            for gate_id in remaining
            if set(gates[gate_id].get("depends_on", ())).issubset(emitted)
        )
        if not ready:
            raise ValueError("gate dependencies contain a cycle")
        gate_id = ready[0]
        emitted.append(gate_id)
        remaining.remove(gate_id)
    return emitted


def run_act_gates(root: Path, act_json: bytes) -> bytes:
    root = _repository_root(root)
    _native.act_validate(act_json)
    document = json.loads(act_json)
    gates = document.get("gates")
    if not isinstance(gates, dict):
        raise ValueError("Act has no valid gate definitions")
    if not gates:
        return b""
    workspace = Path(
        tempfile.mkdtemp(prefix=".archbird-gates-", dir=root.parent)
    )
    try:
        _copy_gate_workspace(root, workspace)
        _apply_materialized_act(workspace, act_json)
        workspace_sha256 = _workspace_sha256(workspace)
        results: dict[str, dict[str, object]] = {}
        for gate_id in _gate_order(gates):
            definition = gates[gate_id]
            if any(
                results[dependency]["status"] != "pass"
                for dependency in definition.get("depends_on", ())
            ):
                empty_sha = hashlib.sha256(b"").hexdigest()
                canonical_definition = _native.json_canonicalize(
                    json.dumps(
                        definition,
                        allow_nan=False,
                        ensure_ascii=True,
                        separators=(",", ":"),
                        sort_keys=True,
                    ).encode("utf-8")
                )
                results[gate_id] = {
                    "id": gate_id,
                    "definition_sha256": hashlib.sha256(
                        canonical_definition
                    ).hexdigest(),
                    "status": "blocked",
                    "exit_code": None,
                    "duration_ms": 0,
                    "environment_sha256": _environment_sha256(
                        _gate_environment(
                            _candidate(
                                workspace,
                                str(definition.get("cwd", ".")),
                            )
                        )
                    ),
                    "stdout_sha256": empty_sha,
                    "stderr_sha256": empty_sha,
                    "stdout_tail_base64": "",
                    "stderr_tail_base64": "",
                }
            else:
                results[gate_id] = _gate_result(gate_id, definition, workspace)
        encoded = json.dumps(
            {
                "workspace_sha256": workspace_sha256,
                "results": [results[gate_id] for gate_id in sorted(results)],
            },
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        return _native.json_canonicalize(encoded)
    finally:
        shutil.rmtree(workspace, ignore_errors=True)


def gate_failure_details(gate_results_json: bytes) -> str:
    if not gate_results_json:
        return ""
    document = json.loads(gate_results_json)
    details: list[str] = []
    for row in document.get("results", []):
        if row.get("status") == "pass":
            continue
        exit_code = row.get("exit_code")
        exit_text = "no exit code" if exit_code is None else f"exit {exit_code}"
        details.append(
            f"Gate {row.get('id', '<unknown>')}: "
            f"{row.get('status', 'error')} ({exit_text}, "
            f"{row.get('duration_ms', 0)} ms)"
        )
        for stream in ("stderr", "stdout"):
            encoded = row.get(f"{stream}_tail_base64", "")
            if not encoded:
                continue
            tail = base64.b64decode(encoded, validate=True).decode(
                "utf-8", errors="replace"
            )
            tail = "".join(
                character
                if character in "\n\t" or ord(character) >= 0x20
                else "?"
                for character in tail
            ).strip()
            if tail:
                details.append(f"{stream} tail:\n{tail[-4096:]}")
                break
    return "\n".join(details)


def render_act(
    root: Path, act_json: bytes, *, format: str, pretty: bool = False
) -> bytes:
    root = _repository_root(root)
    _native.act_validate(act_json)
    if format == "json":
        return _native.json_canonicalize(act_json, pretty=pretty)
    document = json.loads(act_json)
    diffs: list[bytes] = []
    for transition in document["transitions"]:
        kind = transition["kind"]
        path = _relative_path(transition["path"])
        source_path = (
            _relative_path(transition["source_path"])
            if kind == "move"
            else path
        )
        before = b"" if kind == "create" else _read_regular(root, source_path)[0]
        after = (
            b""
            if kind == "delete"
            else base64.b64decode(
                transition["after"]["content_base64"], validate=True
            )
        )
        diffs.append(
            _native.unified_diff(
                before,
                after,
                None if kind == "create" else source_path,
                None if kind == "delete" else path,
            )
        )
    patch = b"".join(diffs)
    if format == "patch":
        return patch
    if format != "markdown":
        raise ValueError("Act format must be markdown, json, or patch")
    acceptance = document["acceptance"]
    lines = [
        "# Accepted Act",
        "",
        f"- Plan: `{document['plan_sha256']}`",
        f"- Transitions: {len(document['transitions'])}",
        f"- Acceptance: `{acceptance['status']}`",
    ]
    constraints = acceptance.get("constraints", [])
    if constraints:
        lines.extend(("", "## Constraints", ""))
        for row in constraints:
            lines.append(f"- `{row['id']}`: `{row['status']}`")
    gates = acceptance.get("gate_results", [])
    if gates:
        lines.extend(("", "## Gates", ""))
        for row in gates:
            lines.append(
                f"- `{row['id']}`: `{row['status']}` "
                f"({row['duration_ms']} ms)"
            )
    if patch:
        lines.extend(("", "## Diff", "", "```diff"))
        lines.append(patch.decode("utf-8", errors="replace").rstrip("\n"))
        lines.append("```")
    return ("\n".join(lines) + "\n").encode("utf-8")


def _write_stage_file(path: Path, data: bytes, mode: int) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(descriptor, mode)
            os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _make_parents(root: Path, relative: str, created: list[Path]) -> None:
    cursor = root
    for part in PurePosixPath(relative).parts[:-1]:
        cursor /= part
        try:
            metadata = cursor.lstat()
        except FileNotFoundError:
            cursor.mkdir(mode=0o755)
            created.append(cursor)
            continue
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            raise OSError(f"unsafe destination parent appeared: {relative}")


def _transition_states(
    act_json: bytes, root: Path
) -> tuple[dict[str, tuple[bytes, int]], dict[str, tuple[bytes, int]]]:
    document = json.loads(act_json)
    initial: dict[str, tuple[bytes, int]] = {}
    final: dict[str, tuple[bytes, int]] = {}
    for transition in document["transitions"]:
        kind = transition["kind"]
        path = _relative_path(transition["path"])
        if kind == "move":
            source = _relative_path(transition["source_path"])
            initial[source] = _read_regular(root, source)
        elif kind != "create":
            initial[path] = _read_regular(root, path)
        if kind != "delete":
            after = transition["after"]
            data = base64.b64decode(after["content_base64"], validate=True)
            mode = 0o755 if after["executable"] else 0o644
            if kind == "modify":
                mode = stat.S_IMODE(initial[path][1])
                mode = mode | 0o111 if after["executable"] else mode & ~0o111
            elif kind == "move":
                source_mode = stat.S_IMODE(initial[transition["source_path"]][1])
                mode = (
                    source_mode | 0o111
                    if after["executable"]
                    else source_mode & ~0o111
                )
            final[path] = (data, mode)
    return initial, final


def _commit_act(root: Path, act_json: bytes) -> None:
    initial, final = _transition_states(act_json, root)
    affected = sorted(set(initial) | set(final))
    stage = Path(tempfile.mkdtemp(prefix=".archbird-act-", dir=root))
    staged_new: dict[str, Path] = {}
    staged_old: dict[str, Path] = {}
    created_directories: list[Path] = []
    committed = False
    mutated = False
    primary_error: BaseException | None = None
    try:
        for index, path in enumerate(affected):
            if path in final:
                data, mode = final[path]
                temporary = stage / f"new-{index}"
                _write_stage_file(temporary, data, mode)
                staged_new[path] = temporary
            if path in initial:
                data, mode = initial[path]
                backup = stage / f"old-{index}"
                _write_stage_file(backup, data, stat.S_IMODE(mode))
                staged_old[path] = backup
        metadata = observe_act_sources(root, act_json)
        if _native.act_preflight_apply(act_json, metadata) != 0:
            raise OSError("Act became already satisfied during commit")
        for path in sorted(staged_new):
            _make_parents(root, path, created_directories)
            _check_parents(root, path)
            os.replace(staged_new[path], _candidate(root, path))
            mutated = True
        for path in sorted(set(initial) - set(final)):
            _check_parents(root, path)
            _candidate(root, path).unlink()
            mutated = True
        committed = True
    except BaseException as error:
        primary_error = error
        raise
    finally:
        rollback_errors: list[str] = []
        if mutated and not committed:
            for path in sorted(set(final) - set(initial), reverse=True):
                candidate = _candidate(root, path)
                try:
                    metadata = candidate.lstat()
                    if not stat.S_ISLNK(metadata.st_mode):
                        candidate.unlink()
                except FileNotFoundError:
                    pass
                except OSError as error:
                    rollback_errors.append(f"remove {path}: {error}")
            for path in sorted(staged_old):
                backup = staged_old[path]
                if not backup.exists():
                    rollback_errors.append(f"restore {path}: backup is missing")
                    continue
                try:
                    _make_parents(root, path, created_directories)
                    os.replace(backup, _candidate(root, path))
                except OSError as error:
                    rollback_errors.append(f"restore {path}: {error}")
        shutil.rmtree(stage, ignore_errors=True)
        if not committed:
            for directory in reversed(created_directories):
                try:
                    directory.rmdir()
                except OSError:
                    pass
        if rollback_errors:
            detail = "; ".join(rollback_errors)
            if primary_error is not None:
                raise OSError(
                    f"{primary_error}; rollback was incomplete: {detail}"
                ) from primary_error
            raise OSError(f"rollback was incomplete: {detail}")


def apply_accepted_act(root: Path, act_json: bytes) -> int:
    root = _repository_root(root)
    metadata = observe_act_sources(root, act_json)
    if _native.act_preflight_apply(act_json, metadata) == 1:
        return 0
    lock_path = root / ".archbird-apply.lock"
    descriptor = os.open(
        lock_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
    )
    try:
        os.write(descriptor, f"{os.getpid()}\n".encode("ascii"))
        os.fsync(descriptor)
        metadata = observe_act_sources(root, act_json)
        if _native.act_preflight_apply(act_json, metadata) == 1:
            return 0
        _commit_act(root, act_json)
    finally:
        os.close(descriptor)
        try:
            lock_path.unlink()
        except FileNotFoundError:
            pass
    document = json.loads(act_json)
    return len(document["transitions"])


__all__ = [
    "apply_accepted_act",
    "observe_act_sources",
    "observe_plan_sources",
    "observe_source_requirements",
    "run_act_gates",
    "gate_failure_details",
    "act_overlay",
    "render_act",
]
