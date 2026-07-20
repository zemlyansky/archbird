"""Compile project-owned coverage reports into strict observed test routes."""

from __future__ import annotations

import ast
import hashlib
import json
import platform
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Mapping

from archbird import __version__, implementation_digest


_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]*$")
_FORMATS = {"coverage.py", "gcov", "istanbul", "llvm"}


class CoverageAdapterError(ValueError):
    """The adapter input cannot support exact per-test symbol evidence."""


def _canonical(value: object) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _safe_relative(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value or "//" in value:
        raise CoverageAdapterError(f"{label} must be a repository-relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise CoverageAdapterError(f"{label} must be a repository-relative path")
    return path.as_posix()


def _exact_object(
    value: object, required: set[str], optional: set[str], label: str
) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise CoverageAdapterError(f"{label} must be an object")
    missing = required - set(value)
    extra = set(value) - required - optional
    if missing or extra:
        raise CoverageAdapterError(
            f"{label} keys differ: missing={sorted(missing)} extra={sorted(extra)}"
        )
    return value


def _sorted_unique(values: object, label: str, *, paths: bool = False) -> list[str]:
    if not isinstance(values, list):
        raise CoverageAdapterError(f"{label} must be an array")
    result = [
        _safe_relative(value, label)
        if paths
        else value
        if isinstance(value, str) and value
        else ""
        for value in values
    ]
    if any(not value for value in result) or result != sorted(
        set(result), key=str.encode
    ):
        raise CoverageAdapterError(f"{label} must be sorted and unique")
    return result


@dataclass(frozen=True)
class _Case:
    context: str | None
    path: str
    report: str
    selector: str


@dataclass(frozen=True)
class _Request:
    cases: tuple[_Case, ...]
    format: str
    group: str
    reports: tuple[tuple[str, str], ...]
    runner_paths: tuple[str, ...]
    raw: Mapping[str, Any]


def _request(value: object) -> _Request:
    root = _exact_object(
        value,
        {
            "artifact",
            "cases",
            "format",
            "group",
            "reports",
            "runner_paths",
            "schema_version",
        },
        set(),
        "coverage request",
    )
    if (
        root["schema_version"] != 1
        or root["artifact"] != "archbird-coverage-observation-request"
    ):
        raise CoverageAdapterError("coverage request has unsupported identity")
    format_name = root["format"]
    if format_name not in _FORMATS:
        raise CoverageAdapterError(f"unsupported coverage format: {format_name!r}")
    group = root["group"]
    if not isinstance(group, str) or not group:
        raise CoverageAdapterError("coverage request group must be non-empty")
    runners = tuple(_sorted_unique(root["runner_paths"], "runner_paths", paths=True))
    if not runners:
        raise CoverageAdapterError(
            "runner_paths must identify project-owned runner configuration"
        )
    reports_value = root["reports"]
    if not isinstance(reports_value, list) or not reports_value:
        raise CoverageAdapterError("reports must be a non-empty array")
    reports: list[tuple[str, str]] = []
    for index, raw in enumerate(reports_value):
        row = _exact_object(raw, {"id", "path"}, set(), f"reports[{index}]")
        report_id = row["id"]
        if not isinstance(report_id, str) or not _ID.fullmatch(report_id):
            raise CoverageAdapterError(f"reports[{index}].id is invalid")
        reports.append(
            (report_id, _safe_relative(row["path"], f"reports[{index}].path"))
        )
    if reports != sorted(
        set(reports), key=lambda row: (row[0].encode(), row[1].encode())
    ):
        raise CoverageAdapterError("reports must be sorted and unique")
    report_ids = {report_id for report_id, _ in reports}
    cases_value = root["cases"]
    if not isinstance(cases_value, list) or not cases_value:
        raise CoverageAdapterError("cases must be a non-empty array")
    cases: list[_Case] = []
    for index, raw in enumerate(cases_value):
        row = _exact_object(
            raw, {"path", "report", "selector"}, {"context"}, f"cases[{index}]"
        )
        path = _safe_relative(row["path"], f"cases[{index}].path")
        report = row["report"]
        selector = row["selector"]
        context = row.get("context")
        if not isinstance(report, str) or report not in report_ids:
            raise CoverageAdapterError(f"cases[{index}].report is unknown")
        if not isinstance(selector, str) or not selector:
            raise CoverageAdapterError(f"cases[{index}].selector must be non-empty")
        if context is not None and (not isinstance(context, str) or not context):
            raise CoverageAdapterError(f"cases[{index}].context must be non-empty")
        if format_name == "coverage.py" and context is None:
            raise CoverageAdapterError(
                "coverage.py cases require an exact dynamic context"
            )
        if format_name != "coverage.py" and context is not None:
            raise CoverageAdapterError(
                f"{format_name} cases must use one isolated report per case"
            )
        cases.append(_Case(context, path, report, selector))

    def key(case: _Case) -> tuple[bytes, bytes]:
        return case.path.encode(), case.selector.encode()

    if cases != sorted(cases, key=key) or len(
        {(case.path, case.selector) for case in cases}
    ) != len(cases):
        raise CoverageAdapterError("cases must be sorted and unique by path/selector")
    if format_name != "coverage.py":
        used_reports = [case.report for case in cases]
        if len(set(used_reports)) != len(used_reports):
            raise CoverageAdapterError(
                f"{format_name} requires one isolated report per case"
            )
    return _Request(tuple(cases), format_name, group, tuple(reports), runners, root)


def _map_index(value: object) -> tuple[str, str, str, dict[str, Mapping[str, Any]]]:
    root = value
    if not isinstance(root, dict) or root.get("artifact") != "map":
        raise CoverageAdapterError("map input must be a canonical Map")
    project = root.get("project")
    evidence = root.get("evidence")
    files = root.get("files")
    if not isinstance(project, str) or not _ID.fullmatch(project):
        raise CoverageAdapterError("Map project identity is invalid")
    if not isinstance(evidence, dict) or not isinstance(files, list):
        raise CoverageAdapterError("Map evidence/files are missing")
    config = evidence.get("config_sha256")
    inputs = evidence.get("input_sha256")
    if not all(
        isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
        for value in (config, inputs)
    ):
        raise CoverageAdapterError("Map digests are invalid")
    indexed: dict[str, Mapping[str, Any]] = {}
    for index, row in enumerate(files):
        if not isinstance(row, dict):
            raise CoverageAdapterError(f"Map files[{index}] is invalid")
        path = _safe_relative(row.get("path"), f"Map files[{index}].path")
        sha = row.get("sha256")
        symbols = row.get("symbols")
        if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise CoverageAdapterError(f"Map files[{index}].sha256 is invalid")
        if not isinstance(symbols, list):
            raise CoverageAdapterError(f"Map files[{index}].symbols is invalid")
        if path in indexed:
            raise CoverageAdapterError(f"duplicate Map file: {path}")
        indexed[path] = row
    return project, config, inputs, indexed


def _report_path(raw: str, repository: Path, files: Mapping[str, Any]) -> str | None:
    candidate = Path(raw)
    if candidate.is_absolute():
        try:
            raw = candidate.resolve().relative_to(repository).as_posix()
        except ValueError:
            return None
    else:
        raw = PurePosixPath(raw).as_posix()
        while raw.startswith("./"):
            raw = raw[2:]
    return raw if raw in files else None


def _map_symbol_at_line(
    file: Mapping[str, Any], line: int, reported_name: str | None = None
) -> str | None:
    matches = [
        row
        for row in file["symbols"]
        if isinstance(row, dict)
        and row.get("line") == line
        and isinstance(row.get("name"), str)
        and row["name"]
    ]
    if reported_name:
        normalized = reported_name.split("(", 1)[0].strip()
        named = [
            row
            for row in matches
            if row["name"] == normalized
            or row["name"].rsplit(".", 1)[-1] == normalized.rsplit("::", 1)[-1]
        ]
        if len(named) == 1:
            return str(named[0]["name"])
    return str(matches[0]["name"]) if len(matches) == 1 else None


class _PythonRanges(ast.NodeVisitor):
    def __init__(self) -> None:
        self.stack: list[str] = []
        self.ranges: list[tuple[int, int, str]] = []

    def _function(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> None:
        name = ".".join((*self.stack, node.name))
        end = getattr(node, "end_lineno", None)
        if isinstance(end, int):
            self.ranges.append((node.lineno, end, name))
        self.stack.append(node.name)
        self.generic_visit(node)
        self.stack.pop()

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        self._function(node)

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self._function(node)

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        self.stack.append(node.name)
        self.generic_visit(node)
        self.stack.pop()


def _coverage_py_hits(
    report: Mapping[str, Any], case: _Case, repository: Path, files: Mapping[str, Any]
) -> dict[tuple[str, str], int]:
    rows = report.get("files")
    if not isinstance(rows, dict):
        raise CoverageAdapterError("coverage.py report has no files object")
    result: dict[tuple[str, str], int] = {}
    for raw_path, coverage in rows.items():
        path = _report_path(str(raw_path), repository, files)
        if path is None or path == case.path or not isinstance(coverage, dict):
            continue
        contexts = coverage.get("contexts")
        if not isinstance(contexts, dict):
            raise CoverageAdapterError(
                "coverage.py report must be generated with --show-contexts"
            )
        executed = {
            int(line)
            for line, names in contexts.items()
            if str(line).isdigit() and isinstance(names, list) and case.context in names
        }
        if not executed:
            continue
        source_path = repository / path
        try:
            source = source_path.read_bytes()
        except OSError as error:
            raise CoverageAdapterError(
                f"cannot read mapped Python source {path}: {error}"
            ) from error
        if _sha256(source) != files[path]["sha256"]:
            raise CoverageAdapterError(f"mapped Python source is stale: {path}")
        try:
            tree = ast.parse(source, filename=path)
        except (SyntaxError, ValueError) as error:
            raise CoverageAdapterError(
                f"cannot parse mapped Python source {path}: {error}"
            ) from error
        ranges = _PythonRanges()
        ranges.visit(tree)
        mapped = {
            str(row["name"])
            for row in files[path]["symbols"]
            if isinstance(row, dict) and isinstance(row.get("name"), str)
        }
        for start, end, name in ranges.ranges:
            if name in mapped and any(start <= line <= end for line in executed):
                result[(path, name)] = 1
    return result


def _istanbul_hits(
    report: Mapping[str, Any], case: _Case, repository: Path, files: Mapping[str, Any]
) -> dict[tuple[str, str], int]:
    result: dict[tuple[str, str], int] = {}
    for raw_path, coverage in report.items():
        path = _report_path(str(raw_path), repository, files)
        if path is None or path == case.path or not isinstance(coverage, dict):
            continue
        functions, counts = coverage.get("fnMap"), coverage.get("f")
        if not isinstance(functions, dict) or not isinstance(counts, dict):
            raise CoverageAdapterError(
                "Istanbul file coverage requires fnMap and f objects"
            )
        for identity, function in functions.items():
            count = counts.get(identity)
            if (
                not isinstance(count, int)
                or isinstance(count, bool)
                or count <= 0
                or not isinstance(function, dict)
            ):
                continue
            location = function.get("decl", function.get("loc"))
            start = location.get("start") if isinstance(location, dict) else None
            line = start.get("line") if isinstance(start, dict) else None
            if not isinstance(line, int) or isinstance(line, bool) or line < 1:
                continue
            name = (
                function.get("name") if isinstance(function.get("name"), str) else None
            )
            symbol = _map_symbol_at_line(files[path], line, name)
            if symbol:
                result[(path, symbol)] = result.get((path, symbol), 0) + count
    return result


def _llvm_hits(
    report: Mapping[str, Any], case: _Case, repository: Path, files: Mapping[str, Any]
) -> dict[tuple[str, str], int]:
    data = report.get("data")
    if report.get("type") != "llvm.coverage.json.export" or not isinstance(data, list):
        raise CoverageAdapterError("LLVM report is not llvm-cov export JSON")
    result: dict[tuple[str, str], int] = {}
    for unit in data:
        functions = unit.get("functions") if isinstance(unit, dict) else None
        if not isinstance(functions, list):
            continue
        for function in functions:
            if not isinstance(function, dict):
                continue
            count = function.get("count")
            names = function.get("filenames")
            regions = function.get("regions")
            if (
                not isinstance(count, int)
                or isinstance(count, bool)
                or count <= 0
                or not isinstance(names, list)
                or not isinstance(regions, list)
            ):
                continue
            name = (
                function.get("name") if isinstance(function.get("name"), str) else None
            )
            for region in regions:
                if not isinstance(region, list) or len(region) < 6:
                    continue
                line, file_id = region[0], region[5]
                if (
                    not isinstance(file_id, int)
                    or not 0 <= file_id < len(names)
                    or not isinstance(line, int)
                ):
                    continue
                path = _report_path(str(names[file_id]), repository, files)
                if path is None or path == case.path:
                    continue
                symbol = _map_symbol_at_line(files[path], line, name)
                if symbol:
                    result[(path, symbol)] = result.get((path, symbol), 0) + count
                    break
    return result


def _gcov_hits(
    report: Mapping[str, Any], case: _Case, repository: Path, files: Mapping[str, Any]
) -> dict[tuple[str, str], int]:
    rows = report.get("files")
    if not isinstance(report.get("format_version"), str) or not isinstance(rows, list):
        raise CoverageAdapterError("gcov report is not --json-format output")
    result: dict[tuple[str, str], int] = {}
    for file in rows:
        if not isinstance(file, dict):
            continue
        raw_path = file.get("file", file.get("file_name"))
        if not isinstance(raw_path, str):
            continue
        path = _report_path(raw_path, repository, files)
        if (
            path is None
            or path == case.path
            or not isinstance(file.get("functions"), list)
        ):
            continue
        for function in file["functions"]:
            if not isinstance(function, dict):
                continue
            count, line = function.get("execution_count"), function.get("start_line")
            if (
                not isinstance(count, int)
                or isinstance(count, bool)
                or count <= 0
                or not isinstance(line, int)
            ):
                continue
            name = function.get("demangled_name", function.get("name"))
            symbol = _map_symbol_at_line(
                files[path], line, name if isinstance(name, str) else None
            )
            if symbol:
                result[(path, symbol)] = result.get((path, symbol), 0) + count
    return result


def compile_test_observations(
    map_json: bytes,
    request_json: bytes,
    *,
    request_directory: Path,
    repository: Path,
) -> bytes:
    """Compile exact per-test reports without running the project."""

    try:
        map_document = json.loads(map_json)
        request_document = json.loads(request_json)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise CoverageAdapterError(f"invalid JSON input: {error}") from error
    request = _request(request_document)
    project, config_sha, map_sha, files = _map_index(map_document)
    repository = repository.resolve()
    report_bytes: dict[str, bytes] = {}
    reports: dict[str, Mapping[str, Any]] = {}
    for report_id, relative in request.reports:
        path = request_directory / relative
        try:
            encoded = path.read_bytes()
            value = json.loads(encoded)
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise CoverageAdapterError(
                f"cannot read coverage report {relative}: {error}"
            ) from error
        if not isinstance(value, dict):
            raise CoverageAdapterError(f"coverage report {relative} must be an object")
        report_bytes[report_id] = encoded
        reports[report_id] = value
    extractor = {
        "coverage.py": _coverage_py_hits,
        "gcov": _gcov_hits,
        "istanbul": _istanbul_hits,
        "llvm": _llvm_hits,
    }[request.format]
    output_cases = []
    subject_paths: set[str] = set()
    for case in request.cases:
        if case.path not in files:
            raise CoverageAdapterError(f"case test path is not mapped: {case.path}")
        hits = extractor(reports[case.report], case, repository, files)
        if not hits:
            raise CoverageAdapterError(
                f"case {case.selector!r} has no exact mapped symbol hits"
            )
        symbols = [
            {"hits": count, "path": path, "symbol": symbol}
            for (path, symbol), count in sorted(
                hits.items(), key=lambda row: (row[0][0].encode(), row[0][1].encode())
            )
        ]
        subject_paths.update(path for path, _ in hits)
        output_cases.append(
            {
                "group": request.group,
                "path": case.path,
                "selector": case.selector,
                "symbols": symbols,
            }
        )
    evidence = []
    for role, paths in (
        ("runner", request.runner_paths),
        ("subject", tuple(sorted(subject_paths, key=str.encode))),
        (
            "test_inventory",
            tuple(sorted({case.path for case in request.cases}, key=str.encode)),
        ),
    ):
        for path in paths:
            if path not in files:
                raise CoverageAdapterError(
                    f"{role} evidence path is not mapped: {path}"
                )
            try:
                current = (repository / path).read_bytes()
            except OSError as error:
                raise CoverageAdapterError(
                    f"cannot read mapped {role} evidence {path}: {error}"
                ) from error
            if _sha256(current) != files[path]["sha256"]:
                raise CoverageAdapterError(f"mapped {role} evidence is stale: {path}")
            evidence.append(
                {"path": path, "role": role, "sha256": files[path]["sha256"]}
            )
    evidence.sort(key=lambda row: (row["role"].encode(), row["path"].encode()))
    configuration = dict(request.raw)
    input_rows = [
        {"id": report_id, "sha256": _sha256(report_bytes[report_id])}
        for report_id, _ in request.reports
    ]
    artifact = {
        "artifact": "archbird-test-symbol-observations",
        "cases": output_cases,
        "producer": {
            "configuration_sha256": _sha256(_canonical(configuration)),
            "implementation_sha256": implementation_digest(),
            "input_sha256": _sha256(_canonical(input_rows)),
            "name": f"archbird-{request.format}-adapter".replace(".", "-"),
            "runtime": f"cpython-{platform.python_version()}",
            "version": __version__,
        },
        "project": project,
        "provenance": "observed",
        "schema_version": 1,
        "source": {
            "config_sha256": config_sha,
            "evidence": evidence,
            "evidence_slice_sha256": _sha256(_canonical(evidence)),
            "map_input_sha256": map_sha,
        },
    }
    return _canonical(artifact) + b"\n"


__all__ = ["CoverageAdapterError", "compile_test_observations"]
