"""Preview and apply source-locked Archbird Plan operations."""

from __future__ import annotations

from collections.abc import Mapping
import copy
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shutil
import stat
import tempfile
from typing import Any, Callable

from . import __version__, implementation_digest
from . import _native
from ._plan_limits import (
    MAX_CHANGE_PATCH_BYTES,
    MAX_COLLECTION_ITEMS,
    MAX_FILE_BYTES,
    MAX_METADATA_BYTES,
    MAX_OPERATION_TEXT_BYTES,
    MAX_PATCH_BYTES,
    MAX_PLAN_BYTES,
    MAX_SAFE_INTEGER,
    MAX_TOUCHED_FILES,
    MAX_TOUCHED_SOURCE_BYTES,
)


_SHA256_LENGTH = 64
_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}$")
_SUPPORTED_ACTIONS = {
    "replace_range",
    "create_file",
    "delete_file",
    "move_file",
}


@dataclass(frozen=True)
class _Diagnostic:
    code: str
    message: str
    item_id: str = ""
    path: str = ""

    def render(self) -> dict[str, object]:
        return {
            "code": self.code,
            "severity": "error",
            "message": self.message,
            "item_id": self.item_id or None,
            "path": self.path or None,
        }


@dataclass(frozen=True)
class _Replacement:
    item_id: str
    path: str
    start: int
    end: int
    before: bytes
    replacement: bytes
    source_sha256: str


@dataclass(frozen=True)
class _FileState:
    data: bytes
    mode: int
    sha256: str


@dataclass
class _Prepared:
    initial: dict[str, _FileState]
    final: dict[str, tuple[bytes, int]]
    changes: list[dict[str, Any]]
    diagnostics: list[_Diagnostic]
    destinations: set[str]


class _AcceptanceRejected(Exception):
    def __init__(self, acceptance: dict[str, object]) -> None:
        super().__init__(f"Plan acceptance is {acceptance['status']}")
        self.acceptance = acceptance


AcceptanceCallback = Callable[
    [Mapping[str, object], Path],
    Mapping[str, object],
]


def _canonical(value: object) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _plan_sha256(plan: Mapping[str, object]) -> str:
    encoded = _canonical(plan)
    return _digest(_native.json_canonicalize(encoded))


def _result(
    plan_sha256: str,
    status: str,
    changes: list[dict[str, Any]],
    diagnostics: list[_Diagnostic],
    acceptance: Mapping[str, object] | None = None,
) -> dict[str, Any]:
    ordered = sorted(
        diagnostics,
        key=lambda row: (row.item_id, row.path, row.code, row.message),
    )
    return {
        "schema_version": 1,
        "artifact": "act-result",
        "provenance": "derived",
        "tool": {
            "name": "archbird",
            "version": __version__,
            "implementation_sha256": implementation_digest(),
        },
        "plan_sha256": plan_sha256,
        "status": status,
        "changes": changes,
        "acceptance": dict(
            acceptance
            or {
                "status": "not_evaluated",
                "verification_sha256": None,
                "constraints": [],
            }
        ),
        "diagnostics": [row.render() for row in ordered],
    }


def _items(plan: Mapping[str, object]) -> list[object]:
    value = plan.get("items")
    if not isinstance(value, list):
        raise ValueError("Plan items must be an array")
    return value


def _operation(item: Mapping[str, object]) -> Mapping[str, object] | None:
    operation = item.get("operation")
    if not isinstance(operation, Mapping):
        return None
    return operation


def _action(operation: Mapping[str, object]) -> str:
    value = operation.get("action")
    return value if isinstance(value, str) else ""


def _item_id(item: Mapping[str, object]) -> str:
    value = item.get("id")
    return value if isinstance(value, str) else ""


def _exact_keys(
    value: Mapping[str, object], expected: set[str], description: str
) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        detail = []
        if missing:
            detail.append(f"missing {', '.join(missing)}")
        if extra:
            detail.append(f"unexpected {', '.join(extra)}")
        raise ValueError(f"{description} has {', '.join(detail)}")


def _valid_id(value: object) -> bool:
    return isinstance(value, str) and _ID.fullmatch(value) is not None


def _valid_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == _SHA256_LENGTH
        and all(character in "0123456789abcdef" for character in value)
    )


def _bounded_text(
    value: object,
    description: str,
    *,
    maximum: int = MAX_METADATA_BYTES,
    nonempty: bool = False,
) -> str:
    if (
        not isinstance(value, str)
        or (nonempty and not value)
        or len(value.encode("utf-8")) > maximum
    ):
        qualifier = "non-empty " if nonempty else ""
        raise ValueError(
            f"{description} must be {qualifier}UTF-8 text of at most "
            f"{maximum} bytes"
        )
    return value


def _bounded_collection(
    value: object,
    description: str,
    *,
    nonempty: bool = False,
) -> list[object]:
    if (
        not isinstance(value, list)
        or (nonempty and not value)
        or len(value) > MAX_COLLECTION_ITEMS
    ):
        qualifier = "a non-empty" if nonempty else "an"
        raise ValueError(
            f"{description} must be {qualifier} array of at most "
            f"{MAX_COLLECTION_ITEMS} items"
        )
    return value


def _validate_ids(
    value: object, description: str, *, nonempty: bool = False
) -> list[str]:
    rows = _bounded_collection(value, description, nonempty=nonempty)
    if (
        any(not _valid_id(row) for row in rows)
        or len(set(rows)) != len(rows)
    ):
        raise ValueError(f"{description} must contain unique stable IDs")
    return [str(row) for row in rows]


def _validate_unique_json_rows(value: list[object], description: str) -> None:
    identities = [_canonical(row) for row in value]
    if len(set(identities)) != len(identities):
        raise ValueError(f"{description} must contain unique values")


def _validate_tool(value: object) -> None:
    if not isinstance(value, Mapping):
        raise ValueError("Plan tool must be an object")
    _exact_keys(
        value,
        {"name", "version", "implementation_sha256"},
        "Plan tool",
    )
    if (
        value["name"] != "archbird"
        or not _valid_sha256(value["implementation_sha256"])
    ):
        raise ValueError("Plan tool identity is invalid")
    _bounded_text(value["version"], "Plan tool version", nonempty=True)


def _validate_source(value: object) -> None:
    if not isinstance(value, Mapping):
        raise ValueError("Plan source must be an object")
    _exact_keys(value, {"project", "map", "verification"}, "Plan source")
    _bounded_text(value["project"], "Plan source project", nonempty=True)
    identities = (
        (
            value["map"],
            {
                "sha256",
                "input_sha256",
                "configuration_sha256",
                "producer_implementation_sha256",
            },
            "Map",
        ),
        (
            value["verification"],
            {"sha256", "policy_sha256", "producer_implementation_sha256"},
            "Verification",
        ),
    )
    for identity, fields, description in identities:
        if not isinstance(identity, Mapping):
            raise ValueError(f"Plan {description} identity must be an object")
        _exact_keys(identity, fields, f"Plan {description} identity")
        if any(not _valid_sha256(identity[field]) for field in fields):
            raise ValueError(f"Plan {description} identity has an invalid SHA-256")


def _validate_operation_shape(operation: object) -> str:
    if not isinstance(operation, Mapping):
        raise ValueError("Plan item operation must be an object")
    action = operation.get("action")
    fields = {
        "replace_range": {
            "action",
            "path",
            "source_sha256",
            "start_byte",
            "end_byte",
            "before",
            "replacement",
        },
        "create_file": {"action", "path", "content"},
        "delete_file": {"action", "path", "source_sha256"},
        "move_file": {
            "action",
            "source_path",
            "destination_path",
            "source_sha256",
        },
        "manual": {"action", "instructions", "candidate_paths"},
    }
    if not isinstance(action, str) or action not in fields:
        raise ValueError("Plan item operation action is unsupported")
    _exact_keys(operation, fields[action], f"{action} operation")
    if action in {"replace_range", "delete_file", "move_file"} and not _valid_sha256(
        operation["source_sha256"]
    ):
        raise ValueError(f"{action} operation has an invalid source_sha256")
    if action == "create_file":
        _bounded_text(
            operation["content"],
            "create_file content",
            maximum=MAX_OPERATION_TEXT_BYTES,
        )
    if action == "replace_range":
        for name in ("start_byte", "end_byte"):
            offset = operation[name]
            if (
                not isinstance(offset, int)
                or isinstance(offset, bool)
                or offset < 0
                or offset > MAX_SAFE_INTEGER
            ):
                raise ValueError(
                    f"replace_range {name} must be a nonnegative safe integer"
                )
        if operation["start_byte"] > operation["end_byte"]:
            raise ValueError("replace_range start_byte must not exceed end_byte")
        _bounded_text(
            operation["before"],
            "replace_range before",
            maximum=MAX_OPERATION_TEXT_BYTES,
        )
        _bounded_text(
            operation["replacement"],
            "replace_range replacement",
            maximum=MAX_OPERATION_TEXT_BYTES,
        )
    if action == "manual":
        _bounded_text(
            operation["instructions"],
            "manual instructions",
            nonempty=True,
        )
        paths = _bounded_collection(
            operation["candidate_paths"],
            "manual candidate_paths",
        )
        if len(set(paths)) != len(paths):
            raise ValueError("manual candidate_paths must be unique")
        for path in paths:
            _safe_relative_path(path)
    return action


def _validate_plan_shape(plan: Mapping[str, object]) -> None:
    root_fields = {
        "schema_version",
        "artifact",
        "provenance",
        "tool",
        "source",
        "objective",
        "items",
        "preserved_constraints",
        "unknowns",
    }
    _exact_keys(plan, root_fields, "Plan")
    if plan["schema_version"] != 1 or plan["artifact"] != "plan":
        raise ValueError("Plan schema_version or artifact is invalid")
    if plan["provenance"] not in {"derived", "asserted"}:
        raise ValueError("Plan provenance is invalid")
    _bounded_text(plan["objective"], "Plan objective", nonempty=True)
    _validate_tool(plan["tool"])
    _validate_source(plan["source"])
    _validate_ids(plan["preserved_constraints"], "preserved_constraints")
    unknowns = _bounded_collection(plan["unknowns"], "Plan unknowns")
    unknown_ids: set[str] = set()
    for unknown in unknowns:
        if not isinstance(unknown, Mapping):
            raise ValueError("Plan unknown must be an object")
        _exact_keys(
            unknown,
            {"id", "statement", "item_id", "constraint_id"},
            "Plan unknown",
        )
        unknown_id = unknown["id"]
        if (
            not _valid_id(unknown_id)
            or unknown_id in unknown_ids
            or (
                unknown["item_id"] is not None
                and not _valid_id(unknown["item_id"])
            )
            or (
                unknown["constraint_id"] is not None
                and not _valid_id(unknown["constraint_id"])
            )
        ):
            raise ValueError("Plan unknown fields are invalid")
        _bounded_text(
            unknown["statement"],
            "Plan unknown statement",
            nonempty=True,
        )
        unknown_ids.add(unknown_id)

    items = _bounded_collection(_items(plan), "Plan items")
    item_ids: set[str] = set()
    dependencies: dict[str, list[str]] = {}
    referenced_unknowns: set[str] = set()
    for raw_item in items:
        if not isinstance(raw_item, Mapping):
            raise ValueError("Plan item must be an object")
        _exact_keys(
            raw_item,
            {
                "id",
                "statement",
                "provenance",
                "origins",
                "evidence",
                "depends_on",
                "operation",
                "acceptance",
                "unknowns",
                "executable",
                "non_executable_reasons",
            },
            "Plan item",
        )
        item_id = raw_item["id"]
        if not _valid_id(item_id) or item_id in item_ids:
            raise ValueError("Plan item IDs must be unique stable IDs")
        item_ids.add(item_id)
        if (
            raw_item["provenance"] not in {"derived", "asserted"}
            or not isinstance(raw_item["executable"], bool)
        ):
            raise ValueError(f"Plan item {item_id} metadata is invalid")
        _bounded_text(
            raw_item["statement"],
            f"Plan item {item_id} statement",
            nonempty=True,
        )
        origins = _bounded_collection(
            raw_item["origins"],
            f"Plan item {item_id} origins",
            nonempty=True,
        )
        _validate_unique_json_rows(origins, f"Plan item {item_id} origins")
        for origin in origins:
            if not isinstance(origin, Mapping):
                raise ValueError(f"Plan item {item_id} origin must be an object")
            _exact_keys(
                origin,
                {
                    "constraint_id",
                    "constraint_result_sha256",
                    "issue_fingerprint",
                },
                "Plan item origin",
            )
            if (
                not _valid_id(origin["constraint_id"])
                or not _valid_sha256(origin["constraint_result_sha256"])
                or not _valid_sha256(origin["issue_fingerprint"])
            ):
                raise ValueError(f"Plan item {item_id} origin is invalid")
        evidence = _bounded_collection(
            raw_item["evidence"],
            f"Plan item {item_id} evidence",
        )
        _validate_unique_json_rows(evidence, f"Plan item {item_id} evidence")
        for row in evidence:
            if not isinstance(row, Mapping):
                raise ValueError(f"Plan item {item_id} evidence must contain objects")
            _exact_keys(
                row,
                {"provenance", "project", "path", "line", "sha256", "detail"},
                "Plan item evidence",
            )
            if (
                row["provenance"] not in {"derived", "asserted", "observed"}
                or not isinstance(row["line"], int)
                or isinstance(row["line"], bool)
                or row["line"] < 0
                or row["line"] > MAX_SAFE_INTEGER
                or (
                    row["sha256"] != "" and not _valid_sha256(row["sha256"])
                )
            ):
                raise ValueError(f"Plan item {item_id} evidence is invalid")
            _bounded_text(
                row["project"],
                f"Plan item {item_id} evidence project",
            )
            _bounded_text(
                row["detail"],
                f"Plan item {item_id} evidence detail",
            )
            if row["path"] != "":
                _safe_relative_path(row["path"])
        dependencies[item_id] = _validate_ids(
            raw_item["depends_on"], f"Plan item {item_id} depends_on"
        )
        item_unknowns = _validate_ids(
            raw_item["unknowns"], f"Plan item {item_id} unknowns"
        )
        referenced_unknowns.update(item_unknowns)
        acceptance = raw_item["acceptance"]
        if not isinstance(acceptance, Mapping):
            raise ValueError(f"Plan item {item_id} acceptance must be an object")
        _exact_keys(acceptance, {"constraints"}, "Plan item acceptance")
        _validate_ids(
            acceptance["constraints"],
            f"Plan item {item_id} acceptance constraints",
            nonempty=True,
        )
        action = _validate_operation_shape(raw_item["operation"])
        reasons = _bounded_collection(
            raw_item["non_executable_reasons"],
            f"Plan item {item_id} non-executable reasons",
        )
        if (
            any(not isinstance(reason, str) or not reason for reason in reasons)
            or len(set(reasons)) != len(reasons)
        ):
            raise ValueError(
                f"Plan item {item_id} non-executable reasons are invalid"
            )
        for reason in reasons:
            _bounded_text(
                reason,
                f"Plan item {item_id} non-executable reason",
                nonempty=True,
            )
        if raw_item["executable"]:
            if reasons or action == "manual":
                raise ValueError(f"Plan item {item_id} has an invalid executable gate")
        elif not reasons:
            raise ValueError(f"Plan item {item_id} requires a blocking reason")

    for item_id, depends_on in dependencies.items():
        if any(dependency not in item_ids for dependency in depends_on):
            raise ValueError(f"Plan item {item_id} has a dangling dependency")
        if item_id in depends_on:
            raise ValueError(f"Plan item {item_id} depends on itself")
    for unknown in unknowns:
        assert isinstance(unknown, Mapping)
        if unknown["item_id"] is not None and unknown["item_id"] not in item_ids:
            raise ValueError(f"Plan unknown {unknown['id']} has a dangling item_id")
    if not referenced_unknowns.issubset(unknown_ids):
        raise ValueError("Plan item has a dangling unknown reference")

    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(item_id: str) -> None:
        if item_id in visiting:
            raise ValueError("Plan item dependency graph contains a cycle")
        if item_id in visited:
            return
        visiting.add(item_id)
        for dependency in dependencies[item_id]:
            visit(dependency)
        visiting.remove(item_id)
        visited.add(item_id)

    for item_id in sorted(item_ids):
        visit(item_id)


def _acceptance_constraint_ids(plan: Mapping[str, object]) -> set[str]:
    identifiers = set(_validate_ids(
        plan["preserved_constraints"],
        "preserved_constraints",
    ))
    for raw_item in _items(plan):
        assert isinstance(raw_item, Mapping)
        acceptance = raw_item["acceptance"]
        assert isinstance(acceptance, Mapping)
        identifiers.update(
            _validate_ids(
                acceptance["constraints"],
                f"Plan item {raw_item['id']} acceptance constraints",
                nonempty=True,
            )
        )
    return identifiers


def _validate_acceptance(
    value: object,
    required_constraint_ids: set[str],
) -> dict[str, object]:
    if not isinstance(value, Mapping):
        raise ValueError("acceptance callback must return an object")
    _exact_keys(
        value,
        {"status", "verification_sha256", "constraints"},
        "Act acceptance",
    )
    status_value = value["status"]
    if status_value not in {"satisfied", "not_satisfied", "unknown"}:
        raise ValueError("applied acceptance status is invalid")
    if not _valid_sha256(value["verification_sha256"]):
        raise ValueError("applied acceptance verification_sha256 is invalid")
    constraints = _bounded_collection(
        value["constraints"],
        "applied acceptance constraints",
    )
    constraint_ids: set[str] = set()
    statuses: list[str] = []
    normalized: list[dict[str, str]] = []
    for row in constraints:
        if not isinstance(row, Mapping):
            raise ValueError("applied acceptance constraint must be an object")
        _exact_keys(row, {"id", "status"}, "Act acceptance constraint")
        identifier = row["id"]
        row_status = row["status"]
        if (
            not _valid_id(identifier)
            or identifier in constraint_ids
            or row_status
            not in {"pass", "fail", "unknown", "waived", "not_applicable"}
        ):
            raise ValueError("applied acceptance constraint is invalid")
        constraint_ids.add(identifier)
        statuses.append(row_status)
        normalized.append({"id": identifier, "status": row_status})
    omitted = sorted(required_constraint_ids - constraint_ids)
    extra = sorted(constraint_ids - required_constraint_ids)
    if omitted or extra:
        details = []
        if omitted:
            details.append(f"omitted {', '.join(omitted)}")
        if extra:
            details.append(f"included unexpected {', '.join(extra)}")
        raise ValueError(
            "applied acceptance constraint coverage is not exact: "
            + "; ".join(details)
        )
    if status_value == "satisfied" and any(
        row not in {"pass", "waived", "not_applicable"} for row in statuses
    ):
        raise ValueError("satisfied acceptance contains an unsatisfied constraint")
    if status_value == "not_satisfied" and "fail" not in statuses:
        raise ValueError("not_satisfied acceptance has no failing constraint")
    if status_value == "unknown" and (
        "unknown" not in statuses or "fail" in statuses
    ):
        raise ValueError("unknown acceptance has contradictory constraints")
    if not normalized and status_value != "satisfied":
        raise ValueError("empty applied acceptance must be satisfied")
    return {
        "status": status_value,
        "verification_sha256": value["verification_sha256"],
        "constraints": normalized,
    }


def _safe_relative_path(value: object) -> str:
    if not isinstance(value, str) or not value or len(value) > 4096:
        raise ValueError("path must be a non-empty string")
    if "\\" in value or "\x00" in value or any(
        ord(character) < 32 for character in value
    ):
        raise ValueError("path must use canonical repository-relative syntax")
    posix = PurePosixPath(value)
    windows = PureWindowsPath(value)
    if (
        posix.is_absolute()
        or windows.is_absolute()
        or windows.drive
        or value != posix.as_posix()
        or any(part in {"", ".", ".."} for part in posix.parts)
    ):
        raise ValueError("path must be canonical and repository-relative")
    return value


def _candidate(root: Path, relative: str) -> Path:
    return root.joinpath(*PurePosixPath(relative).parts)


def _check_path_components(
    root: Path, relative: str, *, include_leaf: bool
) -> None:
    parts = PurePosixPath(relative).parts
    limit = len(parts) if include_leaf else len(parts) - 1
    cursor = root
    for index, part in enumerate(parts[:limit]):
        cursor /= part
        try:
            metadata = cursor.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError(f"path traverses a symlink: {relative}")
        if index < limit - 1 and not stat.S_ISDIR(metadata.st_mode):
            raise ValueError(f"path parent is not a directory: {relative}")


def _read_regular(root: Path, relative: str) -> _FileState:
    _check_path_components(root, relative, include_leaf=True)
    path = _candidate(root, relative)
    try:
        before_open = path.lstat()
    except FileNotFoundError as error:
        raise ValueError(f"source file does not exist: {relative}") from error
    if not stat.S_ISREG(before_open.st_mode):
        raise ValueError(f"source is not a regular file: {relative}")
    if before_open.st_size > MAX_FILE_BYTES:
        raise ValueError(
            f"source file exceeds the {MAX_FILE_BYTES}-byte limit: {relative}"
        )
    flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0)
    flags |= getattr(os, "O_CLOEXEC", 0)
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except FileNotFoundError as error:
        raise ValueError(f"source file does not exist: {relative}") from error
    except OSError as error:
        raise ValueError(
            f"cannot safely open source file {relative}: {error}"
        ) from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(f"source is not a regular file: {relative}")
        if metadata.st_size > MAX_FILE_BYTES:
            raise ValueError(
                f"source file exceeds the {MAX_FILE_BYTES}-byte limit: "
                f"{relative}"
            )
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_FILE_BYTES:
                raise ValueError(
                    f"source file exceeds the {MAX_FILE_BYTES}-byte limit: "
                    f"{relative}"
                )
            chunks.append(chunk)
    finally:
        os.close(descriptor)
    data = b"".join(chunks)
    return _FileState(
        data=data,
        mode=stat.S_IMODE(metadata.st_mode),
        sha256=_digest(data),
    )


def _validate_sha256(value: object) -> str:
    if (
        not isinstance(value, str)
        or len(value) != _SHA256_LENGTH
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValueError("source_sha256 must be a lowercase SHA-256")
    return value


def _existing_state(
    root: Path,
    relative: str,
    initial: dict[str, _FileState],
) -> _FileState:
    if relative not in initial:
        initial[relative] = _read_regular(root, relative)
    return initial[relative]


def _destination_absent(root: Path, relative: str) -> None:
    _check_path_components(root, relative, include_leaf=True)
    try:
        _candidate(root, relative).lstat()
    except FileNotFoundError:
        return
    raise ValueError(f"destination already exists: {relative}")


def _ranges_overlap(left: _Replacement, right: _Replacement) -> bool:
    if left.start == left.end:
        if right.start == right.end:
            return left.start == right.start
        return right.start <= left.start < right.end
    if right.start == right.end:
        return left.start <= right.start < left.end
    return left.start < right.end and right.start < left.end


def _unified_diff(
    before: bytes,
    after: bytes,
    before_path: str | None,
    after_path: str | None,
    *,
    prefix: list[str] | None = None,
) -> str:
    metadata = b"".join(
        f"{line}\n".encode("utf-8") for line in (prefix or [])
    )
    return _native.unified_diff(
        before,
        after,
        before_path,
        after_path,
        metadata=metadata,
    ).decode("utf-8")


def _prepare(plan: Mapping[str, object], root: Path) -> _Prepared:
    diagnostics: list[_Diagnostic] = []
    initial: dict[str, _FileState] = {}
    replacements: dict[str, list[_Replacement]] = {}
    creates: dict[str, tuple[str, bytes]] = {}
    deletes: dict[str, str] = {}
    moves: dict[str, tuple[str, str]] = {}
    destinations: set[str] = set()
    ids: set[str] = set()
    touched_paths: set[str] = set()
    initial_bytes = 0

    def touch(*paths: str) -> None:
        touched_paths.update(paths)
        if len(touched_paths) > MAX_TOUCHED_FILES:
            raise ValueError(
                f"Plan touches more than {MAX_TOUCHED_FILES} files"
            )

    def load_state(relative: str) -> _FileState:
        nonlocal initial_bytes
        existed = relative in initial
        state = _existing_state(root, relative, initial)
        if not existed:
            initial_bytes += len(state.data)
            if initial_bytes > MAX_TOUCHED_SOURCE_BYTES:
                raise ValueError(
                    "Plan touched source exceeds the "
                    f"{MAX_TOUCHED_SOURCE_BYTES}-byte aggregate limit"
                )
        return state

    try:
        items = _items(plan)
    except ValueError as error:
        return _Prepared(
            initial,
            {},
            [],
            [_Diagnostic("invalid_plan", str(error))],
            set(),
        )

    for index, raw_item in enumerate(items):
        if not isinstance(raw_item, Mapping):
            diagnostics.append(
                _Diagnostic(
                    "invalid_item",
                    f"Plan item {index + 1} must be an object",
                    f"item-{index + 1}",
                )
            )
            continue
        item_id = _item_id(raw_item)
        if item_id in ids:
            diagnostics.append(
                _Diagnostic(
                    "duplicate_item_id",
                    f"duplicate item id: {item_id}",
                    item_id,
                )
            )
            continue
        ids.add(item_id)
        operation = _operation(raw_item)
        executable = raw_item.get("executable", True)
        action = _action(operation) if operation is not None else ""
        if executable is not True or action == "manual" or operation is None:
            reasons = raw_item.get("non_executable_reasons")
            suffix = (
                f": {', '.join(str(reason) for reason in reasons)}"
                if isinstance(reasons, list) and reasons
                else ""
            )
            diagnostics.append(
                _Diagnostic(
                    "non_executable",
                    f"item is not executable{suffix}",
                    item_id,
                )
            )
            continue
        if action not in _SUPPORTED_ACTIONS:
            diagnostics.append(
                _Diagnostic(
                    "unsupported_action",
                    f"unsupported action: {action or '<missing>'}",
                    item_id,
                )
            )
            continue
        path = ""
        try:
            if action == "move_file":
                path = _safe_relative_path(operation.get("source_path"))
                destination = _safe_relative_path(
                    operation.get("destination_path")
                )
                touch(path, destination)
            else:
                path = _safe_relative_path(operation.get("path"))
                destination = ""
                touch(path)

            if action == "replace_range":
                state = load_state(path)
                expected = _validate_sha256(operation.get("source_sha256"))
                if state.sha256 != expected:
                    raise ValueError(
                        f"source SHA-256 is stale: expected {expected}, "
                        f"found {state.sha256}"
                    )
                start = operation.get("start_byte")
                end = operation.get("end_byte")
                before = operation.get("before")
                replacement = operation.get("replacement")
                if (
                    not isinstance(start, int)
                    or isinstance(start, bool)
                    or not isinstance(end, int)
                    or isinstance(end, bool)
                    or start < 0
                    or end < start
                    or end > len(state.data)
                ):
                    raise ValueError("replace_range has an invalid UTF-8 byte range")
                if not isinstance(before, str) or not isinstance(replacement, str):
                    raise ValueError(
                        "replace_range before and replacement must be text"
                    )
                try:
                    state.data.decode("utf-8")
                    before_bytes = before.encode("utf-8")
                    replacement_bytes = replacement.encode("utf-8")
                    state.data[:start].decode("utf-8")
                    state.data[start:end].decode("utf-8")
                except UnicodeError as error:
                    raise ValueError(
                        "replace_range offsets must bound valid UTF-8 text"
                    ) from error
                if state.data[start:end] != before_bytes:
                    raise ValueError(
                        "replace_range before text does not match source bytes"
                    )
                replacements.setdefault(path, []).append(
                    _Replacement(
                        item_id,
                        path,
                        start,
                        end,
                        before_bytes,
                        replacement_bytes,
                        expected,
                    )
                )
            elif action == "create_file":
                content = operation.get("content")
                if not isinstance(content, str):
                    raise ValueError("create_file content must be text")
                if path in destinations:
                    raise ValueError(f"destination is claimed more than once: {path}")
                _destination_absent(root, path)
                destinations.add(path)
                creates[path] = (item_id, content.encode("utf-8"))
            elif action == "delete_file":
                state = load_state(path)
                expected = _validate_sha256(operation.get("source_sha256"))
                if state.sha256 != expected:
                    raise ValueError(
                        f"source SHA-256 is stale: expected {expected}, "
                        f"found {state.sha256}"
                    )
                if path in deletes or path in moves:
                    raise ValueError(f"source is consumed more than once: {path}")
                deletes[path] = item_id
            else:
                if path == destination:
                    raise ValueError("move_file source and destination must differ")
                state = load_state(path)
                expected = _validate_sha256(operation.get("source_sha256"))
                if state.sha256 != expected:
                    raise ValueError(
                        f"source SHA-256 is stale: expected {expected}, "
                        f"found {state.sha256}"
                    )
                if path in deletes or path in moves:
                    raise ValueError(f"source is consumed more than once: {path}")
                if destination in destinations:
                    raise ValueError(
                        f"destination is claimed more than once: {destination}"
                    )
                _destination_absent(root, destination)
                destinations.add(destination)
                moves[path] = (item_id, destination)
        except (OSError, ValueError) as error:
            diagnostics.append(
                _Diagnostic(
                    "invalid_operation",
                    str(error),
                    item_id,
                    path,
                )
            )

    for path, edits in replacements.items():
        ordered = sorted(edits, key=lambda row: (row.start, row.end, row.item_id))
        for index, left in enumerate(ordered):
            for right in ordered[index + 1 :]:
                if right.start > left.end:
                    break
                if _ranges_overlap(left, right):
                    diagnostics.append(
                        _Diagnostic(
                            "overlapping_edits",
                            f"replace ranges overlap with item {right.item_id}",
                            left.item_id,
                            path,
                        )
                    )
        if path in deletes:
            diagnostics.append(
                _Diagnostic(
                    "conflicting_actions",
                    "a file cannot be replaced and deleted in the same Plan",
                    deletes[path],
                    path,
                )
            )
        if path in creates:
            diagnostics.append(
                _Diagnostic(
                    "conflicting_actions",
                    "a file cannot be replaced and created in the same Plan",
                    creates[path][0],
                    path,
                )
            )

    final = {path: (state.data, state.mode) for path, state in initial.items()}
    for path, edits in sorted(replacements.items()):
        data = initial[path].data
        for edit in sorted(
            edits, key=lambda row: (row.start, row.end, row.item_id), reverse=True
        ):
            data = data[: edit.start] + edit.replacement + data[edit.end :]
        if len(data) > MAX_FILE_BYTES:
            diagnostics.append(
                _Diagnostic(
                    "resource_limit",
                    f"resulting file exceeds the {MAX_FILE_BYTES}-byte limit",
                    path=path,
                )
            )
        final[path] = (data, initial[path].mode)
    for path in deletes:
        final.pop(path, None)
    for path, (_item_id_value, destination) in moves.items():
        data, mode = final.pop(path)
        final[destination] = (data, mode)
    for path, (_item_id_value, data) in creates.items():
        if len(data) > MAX_FILE_BYTES:
            diagnostics.append(
                _Diagnostic(
                    "resource_limit",
                    f"resulting file exceeds the {MAX_FILE_BYTES}-byte limit",
                    _item_id_value,
                    path,
                )
            )
        final[path] = (data, 0o644)
    final_bytes = sum(len(data) for data, _mode in final.values())
    if final_bytes > MAX_TOUCHED_SOURCE_BYTES:
        diagnostics.append(
            _Diagnostic(
                "resource_limit",
                "Plan resulting source exceeds the "
                f"{MAX_TOUCHED_SOURCE_BYTES}-byte aggregate limit",
            )
        )

    changes: list[dict[str, Any]] = []
    moved_destinations: set[str] = set()
    for source, (item_id, destination) in sorted(moves.items()):
        before = initial[source]
        after_data, _after_mode = final[destination]
        moved_destinations.add(destination)
        prefix = [
            "similarity index 100%",
            f"rename from {source}",
            f"rename to {destination}",
        ]
        if before.data != after_data:
            prefix = [f"rename from {source}", f"rename to {destination}"]
        changes.append(
            {
                "kind": "move",
                "item_ids": sorted(
                    [item_id]
                    + [edit.item_id for edit in replacements.get(source, [])]
                ),
                "source_path": source,
                "path": destination,
                "before_sha256": before.sha256,
                "after_sha256": _digest(after_data),
                "unified_diff": _unified_diff(
                    before.data,
                    after_data,
                    source,
                    destination,
                    prefix=prefix,
                ),
            }
        )
    for path in sorted(set(initial) | set(final)):
        if path in moves or path in moved_destinations:
            continue
        before_state = initial.get(path)
        after_state = final.get(path)
        before = before_state.data if before_state else b""
        after = after_state[0] if after_state else b""
        if before_state is not None and after_state is not None and before == after:
            continue
        if before_state is None:
            kind = "create"
            item_ids = [creates[path][0]]
            prefix = ["new file mode 100644"]
        elif after_state is None:
            kind = "delete"
            item_ids = [deletes[path]]
            prefix = [f"deleted file mode {before_state.mode:06o}"]
        else:
            kind = "modify"
            item_ids = sorted(edit.item_id for edit in replacements[path])
            prefix = None
        changes.append(
            {
                "kind": kind,
                "item_ids": item_ids,
                "path": path,
                "source_path": None,
                "before_sha256": before_state.sha256 if before_state else None,
                "after_sha256": _digest(after) if after_state else None,
                "unified_diff": _unified_diff(
                    before,
                    after,
                    path if before_state else None,
                    path if after_state else None,
                    prefix=prefix,
                ),
            }
        )
    changes.sort(key=lambda row: (row["path"], row["kind"]))
    patch_bytes = 0
    for change in changes:
        rendered_bytes = len(change["unified_diff"].encode("utf-8"))
        if rendered_bytes > MAX_CHANGE_PATCH_BYTES:
            diagnostics.append(
                _Diagnostic(
                    "resource_limit",
                    "rendered file patch exceeds the "
                    f"{MAX_CHANGE_PATCH_BYTES}-byte limit",
                    path=change["path"],
                )
            )
        patch_bytes += rendered_bytes
    if patch_bytes > MAX_PATCH_BYTES:
        diagnostics.append(
            _Diagnostic(
                "resource_limit",
                f"rendered patch exceeds the {MAX_PATCH_BYTES}-byte limit",
            )
        )
    return _Prepared(initial, final, changes, diagnostics, destinations)


def _write_stage_file(path: Path, data: bytes, mode: int) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(descriptor, mode)
            os.fsync(stream.fileno())
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


def _revalidate(root: Path, prepared: _Prepared) -> None:
    for path, expected in prepared.initial.items():
        current = _read_regular(root, path)
        if current.sha256 != expected.sha256:
            raise OSError(f"source changed before apply: {path}")
    for destination in prepared.destinations:
        _destination_absent(root, destination)


def _apply(
    root: Path,
    prepared: _Prepared,
    evaluate_acceptance: Callable[[], dict[str, object]],
) -> dict[str, object]:
    _revalidate(root, prepared)
    affected = sorted(
        {
            str(change.get("source_path") or change["path"])
            for change in prepared.changes
        }
        | {str(change["path"]) for change in prepared.changes}
    )
    stage = Path(tempfile.mkdtemp(prefix=".archbird-act-", dir=root))
    staged_new: dict[str, Path] = {}
    staged_old: dict[str, Path] = {}
    created_directories: list[Path] = []
    committed = False
    mutated = False
    acceptance: dict[str, object] | None = None
    primary_error: BaseException | None = None
    try:
        for index, path in enumerate(affected):
            if path in prepared.final:
                data, mode = prepared.final[path]
                temporary = stage / f"new-{index}"
                _write_stage_file(temporary, data, mode)
                staged_new[path] = temporary
            if path in prepared.initial:
                old = prepared.initial[path]
                backup = stage / f"old-{index}"
                _write_stage_file(backup, old.data, old.mode)
                staged_old[path] = backup
        _revalidate(root, prepared)
        for path in sorted(staged_new):
            _make_parents(root, path, created_directories)
            _check_path_components(root, path, include_leaf=False)
            os.replace(staged_new[path], _candidate(root, path))
            mutated = True
        for path in sorted(set(prepared.initial) - set(prepared.final)):
            _check_path_components(root, path, include_leaf=True)
            _candidate(root, path).unlink()
            mutated = True
        acceptance = evaluate_acceptance()
        if acceptance.get("status") != "satisfied":
            raise _AcceptanceRejected(acceptance)
        committed = True
        return acceptance
    except BaseException as error:
        primary_error = error
        raise
    finally:
        rollback_errors: list[str] = []
        if mutated and not committed:
            for path in sorted(
                set(prepared.final) - set(prepared.initial), reverse=True
            ):
                candidate = _candidate(root, path)
                try:
                    if candidate.exists() and not candidate.is_symlink():
                        candidate.unlink()
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


def _execute(
    plan: Mapping[str, object],
    root: Path,
    *,
    apply: bool,
    verify_acceptance: AcceptanceCallback | None = None,
) -> dict[str, Any]:
    try:
        canonical_plan = _canonical(plan)
        if len(canonical_plan) > MAX_PLAN_BYTES:
            raise ValueError(
                f"Plan exceeds the {MAX_PLAN_BYTES}-byte canonical input limit"
            )
        plan_sha256 = _digest(canonical_plan)
        plan_snapshot = json.loads(canonical_plan)
        if not isinstance(plan_snapshot, dict):
            raise ValueError("Plan must be a JSON object")
    except (TypeError, ValueError) as error:
        fallback = _digest(repr(plan).encode("utf-8", errors="backslashreplace"))
        return _result(
            fallback,
            "blocked",
            [],
            [_Diagnostic("invalid_plan", f"Plan is not canonical JSON: {error}")],
        )
    try:
        _validate_plan_shape(plan_snapshot)
    except ValueError as error:
        return _result(
            plan_sha256,
            "blocked",
            [],
            [_Diagnostic("invalid_plan", str(error))],
        )
    requested_root = Path(root)
    try:
        if requested_root.is_symlink():
            raise ValueError("repository root must not be a symlink")
        repository = requested_root.resolve(strict=True)
        if not repository.is_dir():
            raise ValueError("repository root must be a directory")
        prepared = _prepare(plan_snapshot, repository)
    except (OSError, ValueError) as error:
        return _result(
            plan_sha256,
            "blocked",
            [],
            [_Diagnostic("invalid_root", str(error))],
        )
    if prepared.diagnostics:
        return _result(
            plan_sha256,
            "blocked",
            [],
            prepared.diagnostics,
        )
    if not apply:
        return _result(plan_sha256, "preview", prepared.changes, [])
    required_constraint_ids = _acceptance_constraint_ids(plan_snapshot)
    assert verify_acceptance is not None
    try:
        acceptance = _apply(
            repository,
            prepared,
            lambda: _validate_acceptance(
                verify_acceptance(copy.deepcopy(plan_snapshot), repository),
                required_constraint_ids,
            ),
        )
    except _AcceptanceRejected as error:
        return _result(
            plan_sha256,
            "rejected",
            prepared.changes,
            [
                _Diagnostic(
                    "acceptance_rejected",
                    "Plan changes were rolled back because fresh acceptance is "
                    f"{error.acceptance['status']}.",
                )
            ],
            error.acceptance,
        )
    except Exception as error:
        return _result(
            plan_sha256,
            "failed",
            prepared.changes,
            [
                _Diagnostic(
                    "acceptance_failed",
                    "Plan changes were rolled back because acceptance evaluation "
                    f"failed: {error}",
                )
            ],
        )
    return _result(
        plan_sha256,
        "applied",
        prepared.changes,
        [],
        acceptance,
    )


def preview_plan(plan: Mapping[str, object], root: Path) -> dict[str, Any]:
    """Validate a Plan and return its deterministic patch without writing."""

    return _execute(plan, root, apply=False)


def apply_plan(
    plan: Mapping[str, object],
    root: Path,
    verify_acceptance: AcceptanceCallback,
) -> dict[str, Any]:
    """Apply a Plan, then evaluate its acceptance against the changed tree."""

    if not callable(verify_acceptance):
        raise TypeError("apply_plan requires an acceptance callback")
    return _execute(
        plan,
        root,
        apply=True,
        verify_acceptance=verify_acceptance,
    )


__all__ = ["AcceptanceCallback", "apply_plan", "preview_plan"]
