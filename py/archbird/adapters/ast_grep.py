"""Materialize source-locked Plan operations from an ast-grep preview."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import stat
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Mapping, Sequence, Union


_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")
_VERSION_OUTPUT = re.compile(
    r"^ast-grep(?:\s+version[: ]?)?\s+"
    r"([0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?)$"
)
_LANGUAGE = re.compile(r"^[A-Za-z][A-Za-z0-9+#_.-]{0,63}$")

_HARD_MAX_PATHS = 4096
_HARD_MAX_PATH_BYTES = 256 * 1024
_HARD_MAX_SOURCE_BYTES = 512 * 1024 * 1024
_HARD_MAX_OUTPUT_BYTES = 128 * 1024 * 1024
_HARD_MAX_MATCHES = 1_000_000
_HARD_MAX_TIMEOUT_SECONDS = 600.0


class AstGrepAdapterError(ValueError):
    """ast-grep could not produce a trustworthy bounded rewrite preview."""


@dataclass(frozen=True)
class AstGrepProvenance:
    """Exact executable identity observed by the adapter."""

    path: str
    version: str
    sha256: str


@dataclass(frozen=True)
class _Source:
    bytes: bytes
    path: Path
    sha256: str


@dataclass(frozen=True)
class _ProcessResult:
    returncode: int
    stderr: bytes
    stdout: bytes


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _file_sha256(path: Path, maximum: int = _HARD_MAX_SOURCE_BYTES) -> str:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            if size > maximum:
                raise AstGrepAdapterError(
                    f"file exceeds the {maximum}-byte provenance limit: {path}"
                )
            digest.update(chunk)
    return digest.hexdigest()


def _utf8_size(value: str, label: str) -> int:
    try:
        return len(value.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise AstGrepAdapterError(f"{label} is not valid Unicode text") from error


def _integer(
    value: object,
    label: str,
    *,
    minimum: int,
    maximum: int,
) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < minimum
        or value > maximum
    ):
        raise AstGrepAdapterError(
            f"{label} must be an integer in [{minimum}, {maximum}]"
        )
    return value


def _positive_float(value: object, label: str, maximum: float) -> float:
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or value <= 0
        or value > maximum
    ):
        raise AstGrepAdapterError(f"{label} must be in (0, {maximum}]")
    return float(value)


def _safe_relative(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or "\x00" in value
        or "\\" in value
        or "//" in value
    ):
        raise AstGrepAdapterError(f"{label} must be a repository-relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise AstGrepAdapterError(f"{label} must be a repository-relative path")
    if _utf8_size(value, label) > 4096:
        raise AstGrepAdapterError(f"{label} exceeds 4096 UTF-8 bytes")
    return path.as_posix()


def _exact_object(
    value: object,
    required: set[str],
    optional: set[str],
    label: str,
) -> Mapping[str, object]:
    if not isinstance(value, dict):
        raise AstGrepAdapterError(f"{label} must be an object")
    missing = required - set(value)
    extra = set(value) - required - optional
    if missing or extra:
        raise AstGrepAdapterError(
            f"{label} keys differ: missing={sorted(missing)} "
            f"extra={sorted(extra)}"
        )
    return value


def _regular_path_without_symlinks(root: Path, relative: str) -> Path:
    candidate = root
    for part in PurePosixPath(relative).parts:
        candidate = candidate / part
        try:
            mode = candidate.lstat().st_mode
        except FileNotFoundError as error:
            raise AstGrepAdapterError(
                f"ast-grep input does not exist: {relative}"
            ) from error
        if stat.S_ISLNK(mode):
            raise AstGrepAdapterError(
                f"ast-grep input traverses a symbolic link: {relative}"
            )
    if not stat.S_ISREG(mode):
        raise AstGrepAdapterError(
            f"ast-grep input is not a regular file: {relative}"
        )
    return candidate


def _executable_path(
    value: Union[str, os.PathLike[str]],
) -> Path:
    raw = Path(value)
    if not raw.is_absolute():
        raise AstGrepAdapterError("ast-grep executable must be an absolute path")
    try:
        resolved = raw.resolve(strict=True)
        mode = resolved.stat().st_mode
    except (FileNotFoundError, OSError) as error:
        raise AstGrepAdapterError(
            f"cannot inspect ast-grep executable: {raw}"
        ) from error
    if not stat.S_ISREG(mode):
        raise AstGrepAdapterError("ast-grep executable must be a regular file")
    if not os.access(resolved, os.X_OK):
        raise AstGrepAdapterError("ast-grep executable is not executable")
    return resolved


def _run_bounded(
    arguments: Sequence[str],
    *,
    cwd: Path,
    timeout_seconds: float,
    max_output_bytes: int,
) -> _ProcessResult:
    environment = os.environ.copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "NO_COLOR": "1",
        }
    )
    try:
        process = subprocess.Popen(
            list(arguments),
            cwd=cwd,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=os.name == "posix",
        )
    except OSError as error:
        raise AstGrepAdapterError(f"cannot start ast-grep: {error}") from error
    if process.stdout is None or process.stderr is None:
        process.kill()
        raise AstGrepAdapterError("cannot capture ast-grep output")

    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    lock = threading.Lock()
    output_exceeded = threading.Event()

    def read_stream(name: str, stream: object) -> None:
        while True:
            chunk = stream.read(65536)
            if not chunk:
                return
            with lock:
                used = len(buffers["stdout"]) + len(buffers["stderr"])
                remaining = max_output_bytes - used
                if remaining > 0:
                    buffers[name].extend(chunk[:remaining])
                if len(chunk) > remaining:
                    output_exceeded.set()

    readers = [
        threading.Thread(
            target=read_stream,
            args=("stdout", process.stdout),
            daemon=True,
        ),
        threading.Thread(
            target=read_stream,
            args=("stderr", process.stderr),
            daemon=True,
        ),
    ]
    for reader in readers:
        reader.start()

    def kill_process_tree() -> None:
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass

    deadline = time.monotonic() + timeout_seconds
    timed_out = False
    while process.poll() is None:
        if output_exceeded.is_set():
            kill_process_tree()
            break
        if time.monotonic() >= deadline:
            timed_out = True
            kill_process_tree()
            break
        time.sleep(0.005)
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        kill_process_tree()
        process.wait()
    for reader in readers:
        reader.join(timeout=2.0)
    process.stdout.close()
    process.stderr.close()

    if any(reader.is_alive() for reader in readers):
        kill_process_tree()
        raise AstGrepAdapterError("ast-grep output readers did not terminate")
    if output_exceeded.is_set():
        raise AstGrepAdapterError(
            f"ast-grep output exceeds {max_output_bytes} bytes"
        )
    if timed_out:
        raise AstGrepAdapterError(
            f"ast-grep exceeded its {timeout_seconds:g}-second deadline"
        )
    return _ProcessResult(
        process.returncode,
        bytes(buffers["stderr"]),
        bytes(buffers["stdout"]),
    )


def _parse_version(output: bytes) -> str:
    try:
        text = output.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise AstGrepAdapterError("ast-grep version output is not ASCII") from error
    match = _VERSION_OUTPUT.fullmatch(text)
    if match is None:
        raise AstGrepAdapterError(
            f"ast-grep returned an unrecognized version: {text!r}"
        )
    return match.group(1)


def inspect_ast_grep_executable(
    executable: Union[str, os.PathLike[str]],
    *,
    timeout_seconds: float = 5.0,
) -> AstGrepProvenance:
    """Return the exact path, version, and SHA-256 for an explicit executable."""

    timeout = _positive_float(
        timeout_seconds,
        "timeout_seconds",
        _HARD_MAX_TIMEOUT_SECONDS,
    )
    path = _executable_path(executable)
    before_stat = path.stat()
    before_sha256 = _file_sha256(path)
    result = _run_bounded(
        (str(path), "--version"),
        cwd=path.parent,
        timeout_seconds=timeout,
        max_output_bytes=16384,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise AstGrepAdapterError(
            f"ast-grep --version exited with {result.returncode}: {detail}"
        )
    if result.stderr:
        raise AstGrepAdapterError("ast-grep --version wrote unexpected stderr")
    after_stat = path.stat()
    after_sha256 = _file_sha256(path)
    if (
        before_stat.st_dev,
        before_stat.st_ino,
        before_stat.st_mode,
        before_stat.st_size,
        before_stat.st_mtime_ns,
        before_sha256,
    ) != (
        after_stat.st_dev,
        after_stat.st_ino,
        after_stat.st_mode,
        after_stat.st_size,
        after_stat.st_mtime_ns,
        after_sha256,
    ):
        raise AstGrepAdapterError(
            "ast-grep executable changed while its provenance was inspected"
        )
    return AstGrepProvenance(
        str(path),
        _parse_version(result.stdout),
        before_sha256,
    )


def _language_key(value: str) -> str:
    key = value.lower().replace("-", "").replace("_", "")
    aliases = {
        "c++": "cpp",
        "cxx": "cpp",
        "js": "javascript",
        "jsx": "javascript",
        "py": "python",
        "ts": "typescript",
    }
    return aliases.get(key, key)


def _position(value: object, label: str) -> tuple[int, int]:
    row = _exact_object(value, {"line", "column"}, set(), label)
    line = _integer(row["line"], f"{label}.line", minimum=0, maximum=2**63 - 1)
    column = _integer(
        row["column"],
        f"{label}.column",
        minimum=0,
        maximum=2**63 - 1,
    )
    return line, column


def _operation(
    value: object,
    *,
    index: int,
    language: str,
    sources: Mapping[str, _Source],
) -> dict[str, object]:
    label = f"ast-grep result[{index}]"
    row = _exact_object(
        value,
        {"file", "language", "range", "replacement", "text"},
        {
            "charCount",
            "lines",
            "metaVariables",
            "replacementOffsets",
            "transformed",
        },
        label,
    )
    path = _safe_relative(row["file"], f"{label}.file")
    source = sources.get(path)
    if source is None:
        raise AstGrepAdapterError(
            f"{label}.file is outside the requested path set: {path}"
        )
    reported_language = row["language"]
    if (
        not isinstance(reported_language, str)
        or _language_key(reported_language) != _language_key(language)
    ):
        raise AstGrepAdapterError(
            f"{label}.language does not match requested language {language!r}"
        )
    range_value = _exact_object(
        row["range"],
        {"byteOffset", "end", "start"},
        set(),
        f"{label}.range",
    )
    byte_offset = _exact_object(
        range_value["byteOffset"],
        {"end", "start"},
        set(),
        f"{label}.range.byteOffset",
    )
    start = _integer(
        byte_offset["start"],
        f"{label}.range.byteOffset.start",
        minimum=0,
        maximum=len(source.bytes),
    )
    end = _integer(
        byte_offset["end"],
        f"{label}.range.byteOffset.end",
        minimum=0,
        maximum=len(source.bytes),
    )
    if start >= end:
        raise AstGrepAdapterError(f"{label} must cover a non-empty byte range")
    _position(range_value["start"], f"{label}.range.start")
    _position(range_value["end"], f"{label}.range.end")
    text = row["text"]
    replacement = row["replacement"]
    if not isinstance(text, str) or not isinstance(replacement, str):
        raise AstGrepAdapterError(f"{label}.text/replacement must be strings")
    try:
        matched = source.bytes[start:end].decode("utf-8")
    except UnicodeDecodeError as error:
        raise AstGrepAdapterError(
            f"{label} byte offsets do not bound valid UTF-8 text"
        ) from error
    if matched != text:
        raise AstGrepAdapterError(
            f"{label}.text does not match the source byte range"
        )
    _utf8_size(replacement, f"{label}.replacement")
    if "lines" in row and not isinstance(row["lines"], str):
        raise AstGrepAdapterError(f"{label}.lines must be a string")
    if "charCount" in row:
        char_count = _exact_object(
            row["charCount"],
            {"leading", "trailing"},
            set(),
            f"{label}.charCount",
        )
        _integer(
            char_count["leading"],
            f"{label}.charCount.leading",
            minimum=0,
            maximum=2**63 - 1,
        )
        _integer(
            char_count["trailing"],
            f"{label}.charCount.trailing",
            minimum=0,
            maximum=2**63 - 1,
        )
    for metadata in ("metaVariables", "transformed"):
        if metadata in row and not isinstance(row[metadata], dict):
            raise AstGrepAdapterError(f"{label}.{metadata} must be an object")
    if "replacementOffsets" in row:
        replacement_offsets = _exact_object(
            row["replacementOffsets"],
            {"end", "start"},
            set(),
            f"{label}.replacementOffsets",
        )
        replacement_start = _integer(
            replacement_offsets["start"],
            f"{label}.replacementOffsets.start",
            minimum=0,
            maximum=len(source.bytes),
        )
        replacement_end = _integer(
            replacement_offsets["end"],
            f"{label}.replacementOffsets.end",
            minimum=0,
            maximum=len(source.bytes),
        )
        if (replacement_start, replacement_end) != (start, end):
            raise AstGrepAdapterError(
                f"{label}.replacementOffsets disagree with its matched range"
            )
    return {
        "action": "replace_range",
        "path": path,
        "source_sha256": source.sha256,
        "start_byte": start,
        "end_byte": end,
        "before": text,
        "replacement": replacement,
    }


def materialize_ast_grep_operations(
    repository: Union[str, os.PathLike[str]],
    *,
    executable: Union[str, os.PathLike[str]],
    expected_executable_sha256: str,
    required_version: str,
    pattern: str,
    rewrite: str,
    language: str,
    paths: Sequence[str],
    timeout_seconds: float = 30.0,
    max_source_bytes: int = 64 * 1024 * 1024,
    max_output_bytes: int = 16 * 1024 * 1024,
    max_matches: int = 100_000,
) -> tuple[dict[str, object], ...]:
    """Run a bounded ast-grep preview and return deterministic Plan operations.

    ast-grep is never invoked with a mutation flag. Every emitted edit is
    checked against a pre-execution source snapshot and locked by source SHA-256.
    """

    if not isinstance(expected_executable_sha256, str) or not _SHA256.fullmatch(
        expected_executable_sha256
    ):
        raise AstGrepAdapterError(
            "expected_executable_sha256 must be a lowercase SHA-256"
        )
    if not isinstance(required_version, str) or not _VERSION.fullmatch(
        required_version
    ):
        raise AstGrepAdapterError("required_version must be an exact semver")
    if (
        not isinstance(pattern, str)
        or not pattern
        or _utf8_size(pattern, "pattern") > 1048576
    ):
        raise AstGrepAdapterError("pattern must be 1..1048576 UTF-8 bytes")
    if (
        not isinstance(rewrite, str)
        or _utf8_size(rewrite, "rewrite") > 1048576
    ):
        raise AstGrepAdapterError("rewrite must be at most 1048576 UTF-8 bytes")
    if not isinstance(language, str) or not _LANGUAGE.fullmatch(language):
        raise AstGrepAdapterError("language is invalid")
    timeout = _positive_float(
        timeout_seconds,
        "timeout_seconds",
        _HARD_MAX_TIMEOUT_SECONDS,
    )
    source_limit = _integer(
        max_source_bytes,
        "max_source_bytes",
        minimum=1,
        maximum=_HARD_MAX_SOURCE_BYTES,
    )
    output_limit = _integer(
        max_output_bytes,
        "max_output_bytes",
        minimum=1,
        maximum=_HARD_MAX_OUTPUT_BYTES,
    )
    match_limit = _integer(
        max_matches,
        "max_matches",
        minimum=1,
        maximum=_HARD_MAX_MATCHES,
    )
    if isinstance(paths, (str, bytes)) or not isinstance(paths, Sequence):
        raise AstGrepAdapterError("paths must be a sequence")
    if not paths or len(paths) > _HARD_MAX_PATHS:
        raise AstGrepAdapterError(
            f"paths must contain 1..{_HARD_MAX_PATHS} entries"
        )
    normalized = [_safe_relative(path, "paths[]") for path in paths]
    if len(set(normalized)) != len(normalized):
        raise AstGrepAdapterError("paths must be unique")
    normalized.sort(key=str.encode)
    if sum(_utf8_size(path, "paths[]") + 1 for path in normalized) > (
        _HARD_MAX_PATH_BYTES
    ):
        raise AstGrepAdapterError(
            f"paths exceed {_HARD_MAX_PATH_BYTES} command-line bytes"
        )

    try:
        root = Path(repository).resolve(strict=True)
    except (FileNotFoundError, OSError) as error:
        raise AstGrepAdapterError(
            f"cannot resolve repository root: {repository}"
        ) from error
    if not root.is_dir():
        raise AstGrepAdapterError("repository root must be a directory")

    executable_path = _executable_path(executable)
    provenance = inspect_ast_grep_executable(
        executable_path,
        timeout_seconds=min(timeout, 5.0),
    )
    if provenance.sha256 != expected_executable_sha256:
        raise AstGrepAdapterError(
            "ast-grep executable SHA-256 does not match the reviewed provenance"
        )
    if provenance.version != required_version:
        raise AstGrepAdapterError(
            f"ast-grep version {provenance.version!r} does not match "
            f"required version {required_version!r}"
        )

    sources: dict[str, _Source] = {}
    source_bytes = 0
    for relative in normalized:
        candidate = _regular_path_without_symlinks(root, relative)
        try:
            candidate_size = candidate.stat().st_size
        except OSError as error:
            raise AstGrepAdapterError(
                f"cannot inspect ast-grep input: {relative}"
            ) from error
        if candidate_size > source_limit - source_bytes:
            raise AstGrepAdapterError(
                f"ast-grep inputs exceed {source_limit} bytes"
            )
        content = candidate.read_bytes()
        try:
            content.decode("utf-8")
        except UnicodeDecodeError as error:
            raise AstGrepAdapterError(
                f"ast-grep input is not valid UTF-8: {relative}"
            ) from error
        source_bytes += len(content)
        if source_bytes > source_limit:
            raise AstGrepAdapterError(
                f"ast-grep inputs exceed {source_limit} bytes"
            )
        sources[relative] = _Source(content, candidate, _sha256(content))

    try:
        temporary = tempfile.TemporaryDirectory(
            prefix=".archbird-ast-grep-",
            dir=root.parent,
        )
    except OSError as error:
        raise AstGrepAdapterError(
            f"cannot create an ast-grep preview sandbox beside {root}"
        ) from error
    with temporary:
        sandbox = Path(temporary.name)
        for relative, source in sources.items():
            target = sandbox / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.bytes)
        result = _run_bounded(
            (
                str(executable_path),
                "run",
                "--pattern",
                pattern,
                "--rewrite",
                rewrite,
                "--lang",
                language,
                "--json=compact",
                "--",
                *normalized,
            ),
            cwd=sandbox,
            timeout_seconds=timeout,
            max_output_bytes=output_limit,
        )
        for relative, source in sources.items():
            sandbox_path = sandbox / relative
            if (
                not sandbox_path.is_file()
                or sandbox_path.is_symlink()
                or sandbox_path.read_bytes() != source.bytes
            ):
                raise AstGrepAdapterError(
                    "ast-grep attempted to mutate an input during preview: "
                    f"{relative}"
                )

    for relative, source in sources.items():
        try:
            candidate = _regular_path_without_symlinks(root, relative)
            current = candidate.read_bytes()
        except (AstGrepAdapterError, OSError) as error:
            raise AstGrepAdapterError(
                f"ast-grep changed an input path during preview: {relative}"
            ) from error
        if candidate != source.path or current != source.bytes:
            raise AstGrepAdapterError(
                f"ast-grep mutated an input during preview: {relative}"
            )
    current_executable = _executable_path(executable_path)
    if (
        current_executable != executable_path
        or _file_sha256(current_executable) != provenance.sha256
    ):
        raise AstGrepAdapterError("ast-grep executable changed during preview")

    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise AstGrepAdapterError(
            f"ast-grep exited with {result.returncode}: {detail}"
        )
    if result.stderr:
        raise AstGrepAdapterError("ast-grep preview wrote unexpected stderr")
    try:
        decoded = json.loads(result.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AstGrepAdapterError(
            "ast-grep preview did not return valid UTF-8 JSON"
        ) from error
    if not isinstance(decoded, list):
        raise AstGrepAdapterError("ast-grep preview must return a JSON array")
    if len(decoded) > match_limit:
        raise AstGrepAdapterError(
            f"ast-grep returned more than {match_limit} matches"
        )

    operations = [
        _operation(
            row,
            index=index,
            language=language,
            sources=sources,
        )
        for index, row in enumerate(decoded)
    ]
    operations.sort(
        key=lambda row: (
            str(row["path"]).encode(),
            int(row["start_byte"]),
            int(row["end_byte"]),
            str(row["replacement"]).encode(),
        )
    )
    previous: dict[str, tuple[int, int]] = {}
    seen: set[tuple[object, ...]] = set()
    for operation in operations:
        identity = (
            operation["path"],
            operation["start_byte"],
            operation["end_byte"],
            operation["before"],
            operation["replacement"],
        )
        if identity in seen:
            raise AstGrepAdapterError("ast-grep returned a duplicate rewrite")
        seen.add(identity)
        path = str(operation["path"])
        start = int(operation["start_byte"])
        end = int(operation["end_byte"])
        prior = previous.get(path)
        if prior is not None and start < prior[1]:
            raise AstGrepAdapterError(
                f"ast-grep returned overlapping rewrites for {path}: "
                f"{prior[0]}..{prior[1]} and {start}..{end}"
            )
        previous[path] = (start, end)
    return tuple(operations)
