"""Filesystem transport for native Plan materialization and accepted Acts."""

from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import stat
import tempfile
from types import MappingProxyType
from typing import Mapping

from . import _native


_MAX_FILE_BYTES = 64 * 1024 * 1024


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
    root = root.resolve()
    document = json.loads(requirements_json)
    if not isinstance(document, dict) or set(document) != {"files", "absent"}:
        raise ValueError("native source requirements have an invalid shape")
    files = document["files"]
    absent = document["absent"]
    if not isinstance(files, list) or not isinstance(absent, list):
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


def observe_plan_sources(root: Path, plan_json: bytes) -> bytes:
    requirements = _native.plan_source_requirements(plan_json)
    return observe_source_requirements(root, requirements)


def observe_act_sources(root: Path, act_json: bytes) -> bytes:
    requirements = _native.act_source_requirements(act_json)
    return observe_source_requirements(root, requirements)


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


def render_act(
    root: Path, act_json: bytes, *, format: str, pretty: bool = False
) -> bytes:
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
        _native.act_preflight_apply(act_json, metadata)
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
    root = root.resolve()
    metadata = observe_act_sources(root, act_json)
    _native.act_preflight_apply(act_json, metadata)
    lock_path = root / ".archbird-apply.lock"
    descriptor = os.open(
        lock_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
    )
    try:
        os.write(descriptor, f"{os.getpid()}\n".encode("ascii"))
        os.fsync(descriptor)
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
    "act_overlay",
    "render_act",
]
