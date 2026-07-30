"""Derive source-locked change Plan items from a Verification artifact."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re

from . import __version__, implementation_digest
from . import _native
from ._plan_limits import (
    MAX_COLLECTION_ITEMS,
    MAX_METADATA_BYTES,
    MAX_OPERATION_TEXT_BYTES,
    MAX_PLAN_BYTES,
    MAX_SAFE_INTEGER,
)


_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}$")
_PORTABLE_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_RENAME_FACT_ROLES = {
    "export-origins": "export",
    "exports": "export",
    "imported-names": "import",
    "reexport-candidates": "export",
}


def _canonical_bytes(value: object) -> bytes:
    try:
        rendered = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        return _native.json_canonicalize(rendered)
    except (TypeError, ValueError, _native.Error) as error:
        raise ValueError("Plan inputs must be JSON values") from error


def _sha256(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _sealed_verification_sha256(document: Mapping[str, object]) -> str:
    supplied = document.get("verification_result_sha256")
    if not isinstance(supplied, str) or not _SHA256.fullmatch(supplied):
        raise ValueError("Verification is missing verification_result_sha256")
    unsigned = {
        key: value
        for key, value in document.items()
        if key != "verification_result_sha256"
    }
    actual = _sha256(unsigned)
    if supplied != actual:
        raise ValueError("Verification result digest does not match its content")
    return supplied


def _required_string(
    container: Mapping[str, object], key: str, description: str
) -> str:
    value = container.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"{description} is missing {key}")
    return value


def _required_sha256(
    container: Mapping[str, object], key: str, description: str
) -> str:
    value = _required_string(container, key, description)
    if not _SHA256.fullmatch(value):
        raise ValueError(f"{description} has an invalid {key}")
    return value


def _source_identity(
    map_document: Mapping[str, object],
    verification_document: Mapping[str, object],
) -> dict[str, object]:
    project = _required_string(map_document, "project", "Map")
    map_evidence = map_document.get("evidence")
    map_tool = map_document.get("tool")
    verification_policy = verification_document.get("policy")
    verification_tool = verification_document.get("tool")
    if not isinstance(map_evidence, Mapping) or not isinstance(map_tool, Mapping):
        raise ValueError("Map is missing evidence or producer identity")
    if not isinstance(verification_policy, Mapping) or not isinstance(
        verification_tool, Mapping
    ):
        raise ValueError("Verification is missing policy or producer identity")

    source = {
        "project": project,
        "map": {
            "sha256": _sha256(map_document),
            "input_sha256": _required_sha256(
                map_evidence, "input_sha256", "Map evidence"
            ),
            "configuration_sha256": _required_sha256(
                map_evidence, "config_sha256", "Map evidence"
            ),
            "producer_implementation_sha256": _required_sha256(
                map_tool, "implementation_sha256", "Map tool"
            ),
        },
        "verification": {
            "sha256": _sealed_verification_sha256(verification_document),
            "policy_sha256": _required_sha256(
                verification_policy,
                "constraint_policy_sha256",
                "Verification policy",
            ),
            "producer_implementation_sha256": _required_sha256(
                verification_tool,
                "implementation_sha256",
                "Verification tool",
            ),
        },
    }
    _validate_current_evaluation(source, verification_document)
    return source


def _validate_current_evaluation(
    source: Mapping[str, object], verification_document: Mapping[str, object]
) -> None:
    evaluations = verification_document.get("evaluations")
    if not isinstance(evaluations, list):
        raise ValueError("Verification is missing Map evaluations")
    current = [
        row
        for row in evaluations
        if isinstance(row, Mapping) and row.get("id") == "current"
    ]
    if len(current) != 1:
        raise ValueError("Verification must contain exactly one current Map evaluation")
    map_source = source["map"]
    assert isinstance(map_source, Mapping)
    expected = {
        "project": source["project"],
        "map_input_sha256": map_source["input_sha256"],
        "map_config_sha256": map_source["configuration_sha256"],
        "map_producer_implementation_sha256": map_source[
            "producer_implementation_sha256"
        ],
    }
    for key, value in expected.items():
        if current[0].get(key) != value:
            raise ValueError(f"Verification current Map evaluation mismatches {key}")


def _safe_target(root: Path, relative: str) -> Path | None:
    if not relative or "\\" in relative:
        return None
    parsed = PurePosixPath(relative)
    if (
        parsed.is_absolute()
        or ".." in parsed.parts
        or parsed.as_posix() != relative
        or relative == "."
    ):
        return None
    root_resolved = root.resolve()
    target = root_resolved.joinpath(*parsed.parts)
    try:
        target.resolve().relative_to(root_resolved)
    except ValueError:
        return None
    return target


def _is_repository_path(path: str) -> bool:
    if not path or path == "." or len(path) > 4096 or "\\" in path:
        return False
    parsed = PurePosixPath(path)
    return (
        not parsed.is_absolute()
        and ".." not in parsed.parts
        and "." not in parsed.parts
        and parsed.as_posix() == path
    )


def _map_files(
    map_document: Mapping[str, object],
) -> tuple[dict[str, Mapping[str, object]], set[str]]:
    rows = map_document.get("files")
    if not isinstance(rows, list):
        raise ValueError("Map is missing files")
    index: dict[str, Mapping[str, object]] = {}
    duplicates: set[str] = set()
    for row in rows:
        if not isinstance(row, Mapping) or not isinstance(row.get("path"), str):
            continue
        path = row["path"]
        if path in index:
            duplicates.add(path)
        else:
            index[path] = row
    return index, duplicates


def _policy_results(
    verification_document: Mapping[str, object],
) -> dict[str, Mapping[str, object]]:
    policy = verification_document.get("policy")
    if not isinstance(policy, Mapping):
        return {}
    rows = policy.get("constraints")
    if not isinstance(rows, list):
        return {}
    results = {
        row["id"]: row
        for row in rows
        if isinstance(row, Mapping) and isinstance(row.get("id"), str)
    }
    if len(results) != len(rows):
        raise ValueError(
            "Verification policy constraints must have unique string IDs"
        )
    return results


def _origin(
    constraint_id: str,
    finding: Mapping[str, object] | None,
    policy_result: Mapping[str, object] | None,
) -> dict[str, object]:
    digest = policy_result.get("constraint_result_sha256") if policy_result else None
    if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
        raise ValueError(
            f"Verification policy is missing the result digest for {constraint_id}"
        )
    fingerprint = finding.get("fingerprint") if finding else digest
    if not isinstance(fingerprint, str) or not _SHA256.fullmatch(fingerprint):
        raise ValueError(
            f"Verification issue for {constraint_id} has no valid fingerprint"
        )
    return {
        "constraint_id": constraint_id,
        "constraint_result_sha256": digest,
        "issue_fingerprint": fingerprint,
    }


def _evidence(finding: Mapping[str, object] | None) -> list[dict[str, object]]:
    if not finding or not isinstance(finding.get("evidence"), list):
        return []
    rows: list[dict[str, object]] = []
    identities: set[bytes] = set()
    for row in finding["evidence"]:
        if not isinstance(row, Mapping):
            continue
        normalized = {
            key: copy.deepcopy(row[key])
            for key in ("provenance", "project", "path", "line", "sha256", "detail")
            if key in row
        }
        path = normalized.get("path")
        line = normalized.get("line")
        sha256 = normalized.get("sha256")
        if (
            len(normalized) != 6
            or normalized.get("provenance")
            not in ("derived", "asserted", "observed")
            or not isinstance(normalized.get("project"), str)
            or not isinstance(path, str)
            or (path != "" and not _is_repository_path(path))
            or not isinstance(line, int)
            or isinstance(line, bool)
            or line < 0
            or line > MAX_SAFE_INTEGER
            or not isinstance(sha256, str)
            or (sha256 != "" and not _SHA256.fullmatch(sha256))
            or not isinstance(normalized.get("detail"), str)
        ):
            continue
        identity = _canonical_bytes(normalized)
        if identity not in identities:
            identities.add(identity)
            rows.append(normalized)
    return rows


def _candidate_paths(
    finding: Mapping[str, object] | None,
    actual_definition: Mapping[str, object] | None,
) -> list[str]:
    paths: set[str] = set()
    for row in _evidence(finding):
        path = row.get("path")
        if isinstance(path, str) and _is_repository_path(path):
            paths.add(path)
    if actual_definition:
        for field in ("paths", "source_paths", "target_paths"):
            values = actual_definition.get(field)
            if isinstance(values, list):
                paths.update(
                    value
                    for value in values
                    if isinstance(value, str)
                    and _is_repository_path(value)
                    and not any(character in value for character in "*?[]{}")
                )
    return sorted(paths)


def _constraint_form(
    constraint: Mapping[str, object],
    definitions: Mapping[str, object],
) -> tuple[str, Mapping[str, object] | None]:
    operands = constraint.get("operands")
    if not isinstance(operands, Mapping):
        return "unsupported", None
    actual_name = operands.get("actual")
    actual = definitions.get(actual_name) if isinstance(actual_name, str) else None
    if not isinstance(actual, Mapping):
        return "unsupported", None
    select = actual.get("select")
    assertion = constraint.get("assert")
    if (
        select == "inventory_paths"
        and assertion == "cardinality"
        and operands.get("exact") == 0
    ):
        return "forbidden_paths", actual
    if select == "mapped_paths" and assertion == "required_subset":
        return "required_paths", actual
    if select == "symbols" and assertion in ("disjoint", "set_equal", "subset"):
        return "removable_symbol_set", actual
    if select == "symbols" and assertion == "required_subset":
        return "required_symbols", actual
    if select == "component_membership":
        return "component_membership", actual
    if select == "file_metrics" and actual.get("metric") == "bytes":
        return "max_file_bytes", actual
    known = {
        "component_edges": "component_edges",
        "file_edges": "file_edges",
        "package_entrypoints": "required_package_entrypoint",
        "provider_surface": "provider_surface",
        "test_routes": "test_routes",
    }
    return known.get(str(select), "unsupported"), actual


def _finding_is_current(finding: Mapping[str, object]) -> bool:
    return (
        finding.get("applicability", "applicable") == "applicable"
        and finding.get("disposition", "open") == "open"
        and finding.get("evidence_state", "current") == "current"
    )


def _coalesce_findings(
    findings: Sequence[Mapping[str, object]],
) -> list[dict[str, object]]:
    """Merge multiple renderings of one issue identity before planning."""

    grouped: dict[tuple[str, object], list[Mapping[str, object]]] = {}
    order: list[tuple[str, object]] = []
    for index, finding in enumerate(findings):
        fingerprint = finding.get("fingerprint")
        key: tuple[str, object] = (
            ("fingerprint", fingerprint)
            if isinstance(fingerprint, str)
            else ("row", index)
        )
        if key not in grouped:
            grouped[key] = []
            order.append(key)
        grouped[key].append(finding)

    result: list[dict[str, object]] = []
    for key in order:
        rows = grouped[key]
        semantic_keys = {
            _canonical_bytes(
                {
                    "comparison": row.get("comparison"),
                    "key": row.get("key"),
                }
            )
            for row in rows
        }
        if len(semantic_keys) != 1:
            raise ValueError(
                "Verification reuses one issue fingerprint across distinct "
                "finding keys or comparisons"
            )
        ordered = sorted(
            rows,
            key=lambda row: (
                not _finding_is_current(row),
                _canonical_bytes(row),
            ),
        )
        merged = copy.deepcopy(dict(ordered[0]))
        evidence_by_identity: dict[bytes, dict[str, object]] = {}
        for row in rows:
            for evidence in _evidence(row):
                evidence_by_identity[_canonical_bytes(evidence)] = (
                    copy.deepcopy(evidence)
                )
        merged["evidence"] = [
            evidence_by_identity[identity]
            for identity in sorted(evidence_by_identity)
        ]
        result.append(merged)
    return result


def _destructive_relation_frontier(
    map_document: Mapping[str, object],
) -> str | None:
    definition = {
        "id": "plan-destructive-relations",
        "select": "graph",
        "level": "file",
        "relations": [
            "builds",
            "bridges",
            "calls",
            "declarations",
            "imports",
            "packages",
            "references",
            "tests",
        ],
    }
    try:
        result = json.loads(
            _native.projection_evaluate(
                _canonical_bytes(map_document),
                _canonical_bytes(definition),
            )
        )
    except (_native.Error, json.JSONDecodeError, TypeError, ValueError) as error:
        return f"Destructive relation projection could not be evaluated: {error}."
    completeness = result.get("completeness")
    fact = result.get("fact")
    if (
        result.get("artifact") != "projection-result"
        or not isinstance(fact, Mapping)
        or fact.get("state") != "current"
        or not isinstance(completeness, Mapping)
        or completeness.get("classification") != "complete"
        or completeness.get("exhaustive") is not True
        or completeness.get("truncated") is not False
    ):
        return (
            "Destructive relation evidence is not current, complete, exhaustive, "
            "and untruncated."
        )
    return None


def _operation_id(
    constraint_id: str,
    finding: Mapping[str, object] | None,
    operation: Mapping[str, object],
) -> str:
    identity = {
        "constraint_id": constraint_id,
        "finding_fingerprint": finding.get("fingerprint") if finding else None,
        "operation": operation,
    }
    return "plan-" + _sha256(identity)[:20]


def _unknown_id(item_id: str, statement: str) -> str:
    return "unknown-" + _sha256({"item_id": item_id, "statement": statement})[:20]


def _make_item(
    *,
    constraint_id: str,
    policy_result: Mapping[str, object] | None,
    finding: Mapping[str, object] | None,
    statement: str,
    operation: Mapping[str, object],
    executable: bool,
    reasons: Sequence[str] = (),
) -> tuple[dict[str, object], list[dict[str, object]]]:
    unique_reasons = list(dict.fromkeys(reasons))
    if executable and unique_reasons:
        raise ValueError("Executable Plan items cannot have non-executable reasons")
    if not executable and not unique_reasons:
        raise ValueError("Non-executable Plan items require at least one reason")
    item_id = _operation_id(constraint_id, finding, operation)
    unknown_rows = [
        {
            "id": _unknown_id(item_id, reason),
            "statement": reason,
            "item_id": item_id,
            "constraint_id": constraint_id,
        }
        for reason in unique_reasons
    ]
    item = {
        "id": item_id,
        "statement": statement,
        "provenance": "derived",
        "origins": [_origin(constraint_id, finding, policy_result)],
        "evidence": _evidence(finding),
        "depends_on": [],
        "executable": executable,
        "non_executable_reasons": unique_reasons,
        "operation": dict(operation),
        "acceptance": {"constraints": [constraint_id]},
        "unknowns": [row["id"] for row in unknown_rows],
    }
    return item, unknown_rows


def _path_lock(
    path: str,
    files: Mapping[str, Mapping[str, object]],
    map_document: Mapping[str, object],
) -> tuple[str | None, str | None]:
    hashes: set[str] = set()
    mapped = files.get(path)
    if mapped and isinstance(mapped.get("sha256"), str):
        hashes.add(mapped["sha256"])
    inputs = map_document.get("inputs")
    if isinstance(inputs, list):
        for row in inputs:
            if (
                isinstance(row, Mapping)
                and row.get("path") == path
                and isinstance(row.get("sha256"), str)
            ):
                hashes.add(row["sha256"])
    hashes = {value for value in hashes if _SHA256.fullmatch(value)}
    if not hashes:
        return None, f"No source SHA-256 is available for {path}."
    if len(hashes) != 1:
        return None, f"Map source rows disagree on the source hash for {path}."
    return next(iter(hashes)), None


def _read_locked_source(
    root: Path, path: str, expected_sha256: str
) -> tuple[bytes | None, str | None]:
    target = _safe_target(root, path)
    if target is None:
        return None, f"Repository path {path!r} is unsafe or escapes the project root."
    cursor = root.resolve()
    for part in PurePosixPath(path).parts:
        cursor /= part
        if cursor.is_symlink():
            return None, f"Source path {path} traverses a symbolic link."
    if not target.is_file():
        return None, f"Source path {path} is absent or is not a regular file."
    try:
        source = target.read_bytes()
    except OSError as error:
        return None, f"Source path {path} cannot be read: {error}."
    if hashlib.sha256(source).hexdigest() != expected_sha256:
        return None, f"Source path {path} no longer matches the Map source hash."
    return source, None


def _projected_rename_operation(
    *,
    map_document: Mapping[str, object],
    root: Path,
    symbol: str,
    new_name: str,
    seed_paths: Sequence[str],
) -> tuple[dict[str, object], tuple[str, ...]]:
    old_match = re.search(r"[A-Za-z_][A-Za-z0-9_]*$", symbol)
    if (
        old_match is None
        or _PORTABLE_IDENTIFIER.fullmatch(new_name) is None
        or old_match.group(0) == new_name
    ):
        raise ValueError(
            "rename directives require distinct portable identifier leaves"
        )
    leaf = old_match.group(0)
    projection: dict[str, object] = {
        "select": "symbol_occurrences",
        "names": [symbol],
    }
    paths = sorted(
        {
            path
            for path in seed_paths
            if isinstance(path, str) and _is_repository_path(path)
        }
    )
    if paths:
        projection["paths"] = paths
    request = {"id": "plan-symbol-occurrences", **projection}
    try:
        result = json.loads(
            _native.projection_evaluate(
                _canonical_bytes(map_document),
                _canonical_bytes(request),
            )
        )
    except (_native.Error, json.JSONDecodeError) as error:
        raise ValueError(
            f"symbol occurrence projection could not be evaluated: {error}"
        ) from error
    fact = result.get("fact")
    completeness = result.get("completeness")
    if (
        result.get("artifact") != "projection-result"
        or not isinstance(fact, Mapping)
        or not isinstance(fact.get("items"), list)
        or not isinstance(completeness, Mapping)
        or not _SHA256.fullmatch(str(result.get("projection_result_sha256", "")))
    ):
        raise ValueError("symbol occurrence projection returned an invalid artifact")

    reasons: list[str] = []
    sites: list[dict[str, object]] = []
    seen_sites: set[tuple[str, int, int]] = set()
    for item in fact["items"]:
        if not isinstance(item, Mapping) or not isinstance(
            item.get("attributes"), Mapping
        ):
            raise ValueError("symbol occurrence projection contains an invalid item")
        attributes = item["attributes"]
        path = attributes.get("path")
        start = attributes.get("start_byte")
        end = attributes.get("end_byte")
        if item.get("state") != "current":
            location = path if isinstance(path, str) else "<unknown>"
            message = item.get("message")
            reasons.append(
                f"Rename occurrence at {location} is unknown: "
                f"{message if isinstance(message, str) and message else 'evidence is incomplete'}."
            )
            continue
        if (
            not isinstance(path, str)
            or not isinstance(start, int)
            or isinstance(start, bool)
            or not isinstance(end, int)
            or isinstance(end, bool)
            or start < 0
            or end <= start
        ):
            reasons.append("Current rename evidence has no exact source location.")
            continue
        source_sha256 = attributes.get("source_sha256")
        if not isinstance(source_sha256, str) or not _SHA256.fullmatch(
            source_sha256
        ):
            reasons.append(f"Rename occurrence at {path} has no source lock.")
            continue
        source, read_error = _read_locked_source(root, path, source_sha256)
        if read_error or source is None:
            reasons.append(read_error or f"Source bytes are unavailable for {path}.")
            continue
        if end > len(source) or source[start:end] != leaf.encode("utf-8"):
            reasons.append(
                f"Rename occurrence {path}:{start}-{end} does not select {leaf}."
            )
            continue
        fact_ids = sorted(
            {
                value
                for value in attributes.get("fact_ids", [])
                if isinstance(value, str) and value.startswith("f:")
            }
        )
        providers = sorted(
            {
                value
                for value in attributes.get("providers", [])
                if isinstance(value, str) and value
            }
        )
        if not fact_ids:
            reasons.append(
                f"Rename occurrence {path}:{start}-{end} has no provider fact identity."
            )
            continue
        key = (path, start, end)
        if key in seen_sites:
            raise ValueError("symbol occurrence projection contains duplicate sites")
        seen_sites.add(key)
        sites.append(
            {
                "path": path,
                "source_sha256": source_sha256,
                "start_byte": start,
                "end_byte": end,
                "before": leaf,
                "role": attributes.get("role"),
                "fact_ids": fact_ids,
                "providers": providers,
            }
        )
    counts = completeness.get("counts")
    if not isinstance(counts, Mapping):
        raise ValueError("symbol occurrence projection has no completeness counts")
    unknown = counts.get("unknown")
    unsupported = counts.get("unsupported")
    if (
        not isinstance(unknown, int)
        or isinstance(unknown, bool)
        or unknown < 0
        or not isinstance(unsupported, int)
        or isinstance(unsupported, bool)
        or unsupported < 0
    ):
        raise ValueError("symbol occurrence projection has invalid completeness counts")
    if unsupported:
        reasons.append(
            f"Rename analysis excludes {unsupported} unsupported repository inputs."
        )
    classification = completeness.get("classification")
    exhaustive = completeness.get("exhaustive")
    if (classification != "complete" or exhaustive is not True) and not reasons:
        reasons.append("Rename occurrence evidence is not complete and exhaustive.")
    if not sites:
        reasons.append(f"No exact source sites were available for renaming {symbol}.")
    unique_reasons = tuple(dict.fromkeys(reasons))
    sites.sort(key=lambda row: (str(row["path"]), int(row["start_byte"])))
    return (
        {
            "action": "rename_symbol",
            "symbol": symbol,
            "new_name": new_name,
            "projection": projection,
            "projection_result_sha256": result["projection_result_sha256"],
            "sites": sites,
            "coverage": {
                "classification": classification,
                "exhaustive": exhaustive,
                "selected": len(sites),
                "unknown": unknown,
                "unsupported": unsupported,
            },
        },
        unique_reasons,
    )


def _candidate_targets_path(candidate: object, path: str) -> bool:
    if isinstance(candidate, str):
        return candidate == path
    return isinstance(candidate, Mapping) and candidate.get("path") == path


def _file_consumers(
    map_document: Mapping[str, object], path: str
) -> list[str]:
    consumers: set[str] = set()
    for edge in map_document.get("edges", []):
        if (
            isinstance(edge, Mapping)
            and edge.get("target") == path
            and edge.get("source") != path
            and isinstance(edge.get("source"), str)
        ):
            consumers.add(edge["source"])
    for collection in ("call_resolutions", "symbol_calls", "symbol_references"):
        for row in map_document.get(collection, []):
            if not isinstance(row, Mapping):
                continue
            candidates = row.get("candidates")
            if not isinstance(candidates, list) or not any(
                _candidate_targets_path(candidate, path) for candidate in candidates
            ):
                continue
            source = row.get("source")
            source_path = (
                source.get("path")
                if isinstance(source, Mapping)
                else source if isinstance(source, str) else None
            )
            if isinstance(source_path, str) and source_path != path:
                consumers.add(source_path)

    for build in map_document.get("builds", []):
        if not isinstance(build, Mapping):
            continue
        name = build.get("name")
        label = name if isinstance(name, str) and name else "unknown"
        if build.get("source") == path:
            consumers.add(f"build:{label}:definition")
        paths = build.get("paths")
        if isinstance(paths, list) and path in paths:
            consumers.add(f"build:{label}:path")
        dependencies = build.get("deps")
        if isinstance(dependencies, list) and path in dependencies:
            consumers.add(f"build:{label}:dependency")

    for artifact in map_document.get("artifacts", []):
        if not isinstance(artifact, Mapping):
            continue
        name = artifact.get("name")
        label = name if isinstance(name, str) and name else "unknown"
        if artifact.get("output") == path:
            consumers.add(f"artifact:{label}:output")
        for input_row in artifact.get("inputs", []):
            if isinstance(input_row, Mapping) and input_row.get("path") == path:
                consumers.add(f"artifact:{label}:input")
        for loader in artifact.get("loaded_by", []):
            if isinstance(loader, Mapping) and loader.get("path") == path:
                consumers.add(f"artifact:{label}:loader")
        for build in artifact.get("builds", []):
            if not isinstance(build, Mapping):
                continue
            if build.get("source") == path:
                consumers.add(f"artifact:{label}:build-definition")
            if build.get("target") == path:
                consumers.add(f"artifact:{label}:build-target")

    for surface in map_document.get("surfaces", []):
        if not isinstance(surface, Mapping):
            continue
        name = surface.get("name")
        label = name if isinstance(name, str) and name else "unknown"
        for provider in surface.get("providers", []):
            if isinstance(provider, Mapping) and provider.get("path") == path:
                consumers.add(f"bridge:{label}:provider")
        for surface_name in surface.get("names", []):
            if not isinstance(surface_name, Mapping):
                continue
            candidates = surface_name.get("candidates")
            if isinstance(candidates, list) and path in candidates:
                consumers.add(f"bridge:{label}:implementation")
            for declaration in surface_name.get("declarations", []):
                if (
                    isinstance(declaration, Mapping)
                    and declaration.get("path") == path
                ):
                    consumers.add(f"bridge:{label}:declaration")
            for use in surface_name.get("uses", []):
                if isinstance(use, Mapping) and use.get("path") == path:
                    consumers.add(f"bridge:{label}:use")

    named_entries = map_document.get("named_entries")
    if isinstance(named_entries, Mapping):
        for name, entries in named_entries.items():
            if isinstance(entries, Mapping) and path in entries:
                consumers.add(f"named-entry:{name}")

    for package in map_document.get("packages", []):
        if not isinstance(package, Mapping):
            continue
        name = package.get("name")
        label = name if isinstance(name, str) and name else "unknown"
        if package.get("manifest") == path:
            consumers.add(f"package:{label}:manifest")
        entrypoints = package.get("entrypoints")
        if isinstance(entrypoints, Mapping) and path in entrypoints.values():
            consumers.add(f"package:{label}:entrypoint")
        export_origins = package.get("export_origins")
        if isinstance(export_origins, Mapping) and any(
            isinstance(paths, list) and path in paths
            for paths in export_origins.values()
        ):
            consumers.add(f"package:{label}:export-origin")
        for surface in package.get("entrypoint_surfaces", []):
            if not isinstance(surface, Mapping):
                continue
            if surface.get("path") == path:
                consumers.add(f"package:{label}:entrypoint-surface")
            surface_origins = surface.get("export_origins")
            if isinstance(surface_origins, Mapping) and any(
                isinstance(paths, list) and path in paths
                for paths in surface_origins.values()
            ):
                consumers.add(f"package:{label}:surface-export-origin")

    for test in map_document.get("tests", []):
        if not isinstance(test, Mapping):
            continue
        test_path = test.get("path")
        label = test_path if isinstance(test_path, str) and test_path else "unknown"
        if test_path == path:
            consumers.add(f"test:{label}:file")
        generated_from = test.get("generated_from")
        if isinstance(generated_from, list) and path in generated_from:
            consumers.add(f"test:{label}:generated-from")
        routes = test.get("routes")
        if isinstance(routes, Mapping) and path in routes:
            consumers.add(f"test:{label}:route")
        for route in test.get("route_evidence", []):
            if isinstance(route, Mapping) and route.get("target") == path:
                consumers.add(f"test:{label}:route-evidence")
        for case in test.get("cases", []):
            if not isinstance(case, Mapping):
                continue
            selector = case.get("selector")
            case_label = (
                selector if isinstance(selector, str) and selector else "unknown"
            )
            configured = case.get("configured_routes")
            if isinstance(configured, list) and path in configured:
                consumers.add(f"test:{label}:{case_label}:configured-route")
            case_routes = case.get("routes")
            if isinstance(case_routes, Mapping) and path in case_routes:
                consumers.add(f"test:{label}:{case_label}:route")
            for route in case.get("route_evidence", []):
                if isinstance(route, Mapping) and route.get("target") == path:
                    consumers.add(f"test:{label}:{case_label}:route-evidence")

    parity_path = re.compile(r"@" + re.escape(path) + r"(?::[0-9]+)?$")
    for parity in map_document.get("parity", []):
        if not isinstance(parity, Mapping):
            continue
        name = parity.get("name")
        label = name if isinstance(name, str) and name else "unknown"
        for member in parity.get("members", []):
            if not isinstance(member, Mapping):
                continue
            evidence = member.get("evidence")
            if not isinstance(evidence, Mapping):
                continue
            if any(
                isinstance(locations, list)
                and any(
                    isinstance(location, str)
                    and parity_path.search(location) is not None
                    for location in locations
                )
                for locations in evidence.values()
            ):
                member_name = member.get("label")
                member_label = (
                    member_name
                    if isinstance(member_name, str) and member_name
                    else "unknown"
                )
                consumers.add(f"parity:{label}:{member_label}")
    return sorted(consumers)


def _symbol_consumers(
    map_document: Mapping[str, object], path: str, name: str
) -> list[str]:
    consumers: set[str] = set()
    for file_row in map_document.get("files", []):
        if not isinstance(file_row, Mapping) or file_row.get("path") != path:
            continue
        exports = file_row.get("exports")
        if isinstance(exports, list) and name in exports:
            consumers.add(f"{path}:export")
    for collection in ("symbol_calls", "symbol_references"):
        for row in map_document.get(collection, []):
            if not isinstance(row, Mapping):
                continue
            candidates = row.get("candidates")
            if not isinstance(candidates, list) or not any(
                isinstance(candidate, Mapping)
                and candidate.get("path") == path
                and candidate.get("symbol") == name
                for candidate in candidates
            ):
                continue
            source = row.get("source")
            source_path = source.get("path") if isinstance(source, Mapping) else None
            source_symbol = (
                source.get("symbol") if isinstance(source, Mapping) else None
            )
            if (
                collection == "symbol_references"
                and source_path == path
                and source_symbol == name
            ):
                for candidate in candidates:
                    if (
                        isinstance(candidate, Mapping)
                        and isinstance(candidate.get("path"), str)
                    ):
                        consumers.add(
                            f"{candidate['path']}:{candidate.get('symbol', name)}"
                        )
                continue
            if source_path == path and source_symbol == name:
                continue
            if isinstance(source_path, str):
                consumers.add(
                    f"{source_path}:{source_symbol}"
                    if isinstance(source_symbol, str)
                    else source_path
                )
    for surface in map_document.get("surfaces", []):
        if not isinstance(surface, Mapping):
            continue
        for row in surface.get("names", []):
            if not isinstance(row, Mapping) or row.get("name") != name:
                continue
            candidates = row.get("candidates")
            if isinstance(candidates, list) and path in candidates:
                consumers.add(f"surface:{surface.get('name', 'unknown')}")
    return sorted(consumers)


def _forbidden_path_item(
    *,
    root: Path,
    map_document: Mapping[str, object],
    files: Mapping[str, Mapping[str, object]],
    duplicate_paths: set[str],
    constraint_id: str,
    policy_result: Mapping[str, object] | None,
    finding: Mapping[str, object],
    relation_frontier_error: str | None,
) -> tuple[dict[str, object], list[dict[str, object]]]:
    key = finding.get("key")
    if not isinstance(key, str) or not key:
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove the path forbidden by {constraint_id}.",
            instructions="Identify the exact forbidden path and remove it.",
            reasons=("Verification did not identify one exact forbidden path.",),
        )
    if not _is_repository_path(key):
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove the unsafe path reported by {constraint_id}.",
            instructions=(
                "Resolve the unsafe path evidence without using it as a "
                "filesystem path."
            ),
            reasons=(f"Verification path {key!r} is not repository-relative.",),
        )
    source_sha256, lock_error = _path_lock(key, files, map_document)
    reasons: list[str] = []
    if key in duplicate_paths:
        reasons.append(f"Map contains duplicate file rows for {key}.")
    if lock_error:
        reasons.append(lock_error)
    if relation_frontier_error:
        reasons.append(relation_frontier_error)
    if source_sha256:
        _, read_error = _read_locked_source(root, key, source_sha256)
        if read_error:
            reasons.append(read_error)
    consumers = _file_consumers(map_document, key)
    if consumers:
        reasons.append(
            f"Known consumers of {key} require a reviewed rewrite: "
            + ", ".join(consumers)
            + "."
        )
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove forbidden path {key}.",
            instructions=(
                f"Rewrite or remove the named consumers of {key}, then remove "
                "the source-locked path."
            ),
            reasons=tuple(reasons),
            candidate_paths=(key,),
        )
    if not source_sha256:
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove forbidden path {key}.",
            instructions=f"Remove {key} after establishing an exact source lock.",
            reasons=tuple(reasons),
            candidate_paths=(key,),
        )
    return _make_item(
        constraint_id=constraint_id,
        policy_result=policy_result,
        finding=finding,
        statement=f"Delete forbidden path {key}.",
        operation={
            "action": "delete_file",
            "path": key,
            "source_sha256": source_sha256,
        },
        executable=not reasons,
        reasons=tuple(reasons),
    )


def _forbidden_path_items(
    *,
    root: Path,
    map_document: Mapping[str, object],
    verification_document: Mapping[str, object],
    files: Mapping[str, Mapping[str, object]],
    duplicate_paths: set[str],
    constraint: Mapping[str, object],
    policy_result: Mapping[str, object] | None,
    relation_frontier_error: str | None,
) -> list[tuple[dict[str, object], list[dict[str, object]]]]:
    constraint_id = constraint["id"]
    assert isinstance(constraint_id, str)
    constraint_operands = constraint.get("operands")
    actual_name = (
        constraint_operands.get("actual")
        if isinstance(constraint_operands, Mapping)
        else None
    )
    operand_rows = verification_document.get("operands")
    actual_matches = (
        [
            row
            for row in operand_rows
            if isinstance(row, Mapping) and row.get("name") == actual_name
        ]
        if isinstance(operand_rows, list) and isinstance(actual_name, str)
        else []
    )
    findings = constraint.get("findings")
    finding_rows = (
        _coalesce_findings(
            [row for row in findings if isinstance(row, Mapping)]
        )
        if isinstance(findings, list)
        else []
    )
    fallback_finding = finding_rows[0] if finding_rows else None

    def manual(
        reason: str, candidate_paths: Sequence[str] = ()
    ) -> list[tuple[dict[str, object], list[dict[str, object]]]]:
        return [
            _manual_item(
                constraint_id=constraint_id,
                policy_result=policy_result,
                finding=fallback_finding,
                statement=f"Remove paths forbidden by {constraint_id}.",
                instructions=(
                    "Review the exhaustive inventory-path operand and provide "
                    "source-locked deletions for each forbidden path."
                ),
                reasons=(reason,),
                candidate_paths=candidate_paths,
            )
        ]

    if len(actual_matches) != 1:
        return manual(
            "Verification does not contain one exact actual ProjectionResult "
            f"for {constraint_id}."
        )
    actual = actual_matches[0]
    completeness = actual.get("completeness")
    if (
        actual.get("state") != "current"
        or actual.get("shape") != "set"
        or not isinstance(completeness, Mapping)
        or completeness.get("classification") != "complete"
        or completeness.get("exhaustive") is not True
        or completeness.get("truncated") is not False
    ):
        return manual(
            "The forbidden-path actual ProjectionResult is not current, "
            "complete, exhaustive, and untruncated."
        )
    actual_sha256 = actual.get("sha256")
    if not isinstance(actual_sha256, str) or not _SHA256.fullmatch(actual_sha256):
        return manual("The forbidden-path actual ProjectionResult has no identity.")
    correlated_findings = []
    for finding in finding_rows:
        if not _finding_is_current(finding):
            continue
        if any(
            row.get("sha256") == actual_sha256
            or (
                isinstance(row.get("detail"), str)
                and isinstance(actual_name, str)
                and actual_name in row["detail"]
            )
            for row in _evidence(finding)
        ):
            correlated_findings.append(finding)
    if len(correlated_findings) != 1:
        return manual(
            "Verification does not contain one current issue correlated to the "
            "forbidden-path actual ProjectionResult."
        )
    finding = correlated_findings[0]
    items = actual.get("items")
    if not isinstance(items, list) or not items:
        return manual(
            "The failing forbidden-path ProjectionResult contains no path items."
        )
    concrete: list[tuple[str, list[dict[str, object]]]] = []
    for row in items:
        if (
            not isinstance(row, Mapping)
            or row.get("state") != "current"
            or not isinstance(row.get("key"), str)
            or not _is_repository_path(row["key"])
        ):
            return manual(
                "The forbidden-path ProjectionResult contains a non-current or "
                "non-concrete path item."
            )
        item_finding = dict(finding)
        item_finding["evidence"] = [
            *(
                row["evidence"]
                if isinstance(row.get("evidence"), list)
                else []
            ),
            *(
                finding["evidence"]
                if isinstance(finding.get("evidence"), list)
                else []
            ),
        ]
        evidence = _evidence(item_finding)
        path = row["key"]
        if not any(item.get("path") == path for item in evidence):
            return manual(
                f"Projection item {path} has no directly correlated path evidence.",
                (path,),
            )
        concrete.append((path, evidence))
    paths = [path for path, _ in concrete]
    if len(set(paths)) != len(paths):
        return manual(
            "The forbidden-path ProjectionResult contains duplicate path items.",
            paths,
        )

    results = []
    for path, evidence in sorted(concrete):
        item_finding = dict(finding)
        item_finding["key"] = path
        item_finding["evidence"] = evidence
        results.append(
            _forbidden_path_item(
                root=root,
                map_document=map_document,
                files=files,
                duplicate_paths=duplicate_paths,
                constraint_id=constraint_id,
                policy_result=policy_result,
                finding=item_finding,
                relation_frontier_error=relation_frontier_error,
            )
        )
    return results


def _symbol_candidates(
    files: Mapping[str, Mapping[str, object]],
    finding: Mapping[str, object],
    name: str,
) -> list[tuple[str, Mapping[str, object]]]:
    evidence_paths = {
        row["path"]
        for row in _evidence(finding)
        if isinstance(row.get("path"), str) and row.get("path")
    }
    candidates: list[tuple[str, Mapping[str, object]]] = []
    for path, file_row in files.items():
        if evidence_paths and path not in evidence_paths:
            continue
        symbols = file_row.get("symbols")
        if not isinstance(symbols, list):
            continue
        for symbol in symbols:
            if isinstance(symbol, Mapping) and symbol.get("name") == name:
                candidates.append((path, symbol))
    return candidates


def _complete_symbol_operands(
    verification_document: Mapping[str, object],
    constraint: Mapping[str, object],
) -> tuple[Mapping[str, object] | None, str | None]:
    operands = constraint.get("operands")
    if not isinstance(operands, Mapping):
        return None, "Constraint does not identify its symbol operands."
    mapping = operands.get("mapping")
    if mapping not in (None, ""):
        return None, (
            "Mapped symbol identities do not identify the declaration that may "
            "be removed."
        )
    evaluated = verification_document.get("operands")
    if not isinstance(evaluated, list):
        return None, "Verification does not contain evaluated symbol operands."
    selected: dict[str, Mapping[str, object]] = {}
    for role in ("actual", "expected"):
        name = operands.get(role)
        if not isinstance(name, str) or not name:
            return None, f"Constraint does not identify its {role} symbol operand."
        matches = [
            row
            for row in evaluated
            if isinstance(row, Mapping) and row.get("name") == name
        ]
        if len(matches) != 1:
            return None, (
                f"Verification does not contain one exact {role} symbol "
                f"ProjectionResult for {constraint.get('id', 'constraint')}."
            )
        result = matches[0]
        completeness = result.get("completeness")
        if (
            result.get("state") != "current"
            or result.get("shape") != "set"
            or not isinstance(result.get("items"), list)
            or not isinstance(result.get("sha256"), str)
            or not _SHA256.fullmatch(result["sha256"])
            or not isinstance(completeness, Mapping)
            or completeness.get("classification") != "complete"
            or completeness.get("exhaustive") is not True
            or completeness.get("truncated") is not False
        ):
            return None, (
                f"The {role} symbol ProjectionResult is not current, complete, "
                "exhaustive, untruncated, and independently identified."
            )
        selected[role] = result
    return selected["actual"], None


def _removable_symbol_finding(
    constraint: Mapping[str, object], finding: Mapping[str, object]
) -> bool:
    assertion = constraint.get("assert")
    comparison = finding.get("comparison")
    if assertion == "disjoint":
        return comparison in ("extra", "overlap")
    return assertion in ("set_equal", "subset") and comparison == "extra"


def _correlate_symbol_finding(
    actual: Mapping[str, object], finding: Mapping[str, object]
) -> tuple[Mapping[str, object] | None, str | None]:
    name = finding.get("key")
    if not isinstance(name, str) or not name:
        return None, "Verification did not identify one exact removable symbol."
    items = actual.get("items")
    assert isinstance(items, list)
    matches = [
        row
        for row in items
        if isinstance(row, Mapping) and row.get("key") == name
    ]
    if len(matches) != 1 or matches[0].get("state") != "current":
        return None, (
            f"Verification issue {name} does not correlate to one current "
            "actual symbol projection item."
        )
    projected_evidence = matches[0].get("evidence")
    merged = dict(finding)
    merged["evidence"] = [
        *(projected_evidence if isinstance(projected_evidence, list) else []),
        *(
            finding["evidence"]
            if isinstance(finding.get("evidence"), list)
            else []
        ),
    ]
    if not any(
        isinstance(row.get("path"), str) and row.get("path")
        for row in _evidence(merged)
    ):
        return None, (
            f"Actual symbol projection item {name} has no repository source "
            "witness."
        )
    return merged, None


def _removable_symbol_item(
    *,
    root: Path,
    map_document: Mapping[str, object],
    files: Mapping[str, Mapping[str, object]],
    duplicate_paths: set[str],
    constraint_id: str,
    policy_result: Mapping[str, object] | None,
    finding: Mapping[str, object],
    relation_frontier_error: str | None,
) -> tuple[dict[str, object], list[dict[str, object]]]:
    name = finding.get("key")
    if not isinstance(name, str) or not name:
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove the symbol selected by {constraint_id}.",
            instructions="Identify and remove the exact unexpected declaration.",
            reasons=("Verification did not identify one exact removable symbol.",),
        )
    candidates = _symbol_candidates(files, finding, name)
    if len(candidates) != 1:
        paths = sorted({path for path, _ in candidates})
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove symbol {name} to satisfy {constraint_id}.",
            instructions=(
                f"Choose and remove every unexpected declaration of symbol {name}."
            ),
            reasons=(
                f"Expected one exact declaration extent for {name}, found "
                f"{len(candidates)}.",
            ),
            candidate_paths=paths,
        )
    path, symbol = candidates[0]
    reasons: list[str] = []
    extent = symbol.get("extent")
    if (
        not isinstance(extent, Mapping)
        or not isinstance(extent.get("start"), int)
        or isinstance(extent.get("start"), bool)
        or not isinstance(extent.get("end"), int)
        or isinstance(extent.get("end"), bool)
        or extent["start"] < 0
        or extent["start"] > MAX_SAFE_INTEGER
        or extent["end"] <= extent["start"]
        or extent["end"] > MAX_SAFE_INTEGER
    ):
        reasons.append(f"Map has no exact declaration extent for {name}.")
    if symbol.get("syntax_recovery"):
        reasons.append(f"Declaration {name} was recovered from invalid syntax.")
    if "." in name or symbol.get("kind") == "method":
        reasons.append(
            f"Removing nested declaration {name} requires a contextual syntax rewrite."
        )
    if relation_frontier_error:
        reasons.append(relation_frontier_error)
    if path in duplicate_paths:
        reasons.append(f"Map contains duplicate file rows for {path}.")
    source_sha256, lock_error = _path_lock(path, files, map_document)
    if lock_error:
        reasons.append(lock_error)
    source: bytes | None = None
    if source_sha256:
        source, read_error = _read_locked_source(root, path, source_sha256)
        if read_error:
            reasons.append(read_error)
    consumers = _symbol_consumers(map_document, path, name)
    if consumers:
        reasons.append(
            f"Known consumers of {name} require a reviewed rewrite: "
            + ", ".join(consumers)
            + "."
        )
    before: str | None = None
    if source is not None and isinstance(extent, Mapping):
        start = extent.get("start")
        end = extent.get("end")
        if (
            isinstance(start, int)
            and not isinstance(start, bool)
            and isinstance(end, int)
            and not isinstance(end, bool)
            and end <= len(source)
        ):
            try:
                before = source[start:end].decode("utf-8")
            except UnicodeDecodeError:
                reasons.append(
                    f"Declaration extent for {name} is not valid UTF-8 text."
                )
        else:
            reasons.append(f"Declaration extent for {name} exceeds {path}.")
    if before is None or source_sha256 is None or not isinstance(extent, Mapping):
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Remove symbol {name} to satisfy {constraint_id}.",
            instructions=(
                f"Remove {name} after selecting one complete, source-locked "
                "declaration extent and reviewing its consumers."
            ),
            reasons=tuple(dict.fromkeys(reasons)),
            candidate_paths=(path,),
        )
    operation = {
        "action": "replace_range",
        "path": path,
        "source_sha256": source_sha256,
        "start_byte": extent["start"],
        "end_byte": extent["end"],
        "before": before,
        "replacement": "",
    }
    return _make_item(
        constraint_id=constraint_id,
        policy_result=policy_result,
        finding=finding,
        statement=f"Remove symbol {name} from {path} to satisfy {constraint_id}.",
        operation=operation,
        executable=not reasons,
        reasons=tuple(dict.fromkeys(reasons)),
    )


def _manual_item(
    *,
    constraint_id: str,
    policy_result: Mapping[str, object] | None,
    finding: Mapping[str, object] | None,
    statement: str,
    instructions: str,
    reasons: Sequence[str],
    candidate_paths: Sequence[str] = (),
) -> tuple[dict[str, object], list[dict[str, object]]]:
    operation = {
        "action": "manual",
        "instructions": instructions,
        "candidate_paths": sorted(
            {
                path
                for path in candidate_paths
                if isinstance(path, str) and _is_repository_path(path)
            }
        ),
    }
    return _make_item(
        constraint_id=constraint_id,
        policy_result=policy_result,
        finding=finding,
        statement=statement,
        operation=operation,
        executable=False,
        reasons=reasons,
    )


def _non_executable_item(
    *,
    form: str,
    constraint_id: str,
    policy_result: Mapping[str, object] | None,
    finding: Mapping[str, object] | None,
    actual_definition: Mapping[str, object] | None,
) -> tuple[dict[str, object], list[dict[str, object]]]:
    key = finding.get("key") if finding else None
    subject = key if isinstance(key, str) and key else constraint_id
    paths = _candidate_paths(finding, actual_definition)
    if form == "required_paths":
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Create required path {subject}.",
            instructions=(
                f"Provide reviewed content for {subject}, then replace this manual "
                "operation with create_file."
            ),
            reasons=(
                f"Verification requires {subject} but does not specify file content.",
            ),
            candidate_paths=(subject,) if isinstance(subject, str) else (),
        )
    if form == "required_symbols":
        return _manual_item(
            constraint_id=constraint_id,
            policy_result=policy_result,
            finding=finding,
            statement=f"Add required symbol {subject}.",
            instructions=(
                f"Provide the signature, implementation, and destination for "
                f"{subject}."
            ),
            reasons=(
                f"Verification requires {subject} but does not define its code.",
            ),
            candidate_paths=paths,
        )
    if form == "component_membership":
        instructions = (
            "Select an exact file move or an exact archbird.json component-path "
            "edit after reviewing ownership overlaps."
        )
        reason = (
            "Component membership evidence does not uniquely determine a file move "
            "or configuration edit."
        )
    elif form == "max_file_bytes":
        instructions = (
            "Select declarations to extract or simplify, including their destination "
            "and reference rewrites."
        )
        reason = "A file-size violation does not determine a behavior-preserving edit."
    elif form in ("file_edges", "component_edges"):
        instructions = (
            "Select a replacement dependency route and provide exact source rewrites."
        )
        reason = "Dependency evidence does not identify the intended replacement route."
    elif form == "required_package_entrypoint":
        instructions = (
            "Select the package manifest entrypoint edit and its reviewed target."
        )
        reason = "Entrypoint evidence does not provide an exact manifest edit."
    elif form == "provider_surface":
        instructions = (
            "Provide the reviewed bridge or registration transformation for this "
            "provider surface."
        )
        reason = "Provider evidence does not define bridge code or registration syntax."
    elif form == "test_routes":
        instructions = "Provide the reviewed test route or test template to add."
        reason = "Route evidence does not define test code or registration syntax."
    else:
        instructions = (
            "Review the Verification evidence and provide a source-locked "
            "transformation."
        )
        reason = "This compiled constraint form has no deterministic edit operator."
    return _manual_item(
        constraint_id=constraint_id,
        policy_result=policy_result,
        finding=finding,
        statement=f"Resolve {constraint_id}: {subject}.",
        instructions=instructions,
        reasons=(reason,),
        candidate_paths=paths,
    )


def _apply_rename_directives(
    *,
    map_document: Mapping[str, object],
    root: Path,
    ordered_checks: Sequence[Mapping[str, object]],
    policy_results: Mapping[str, Mapping[str, object]],
    items: list[dict[str, object]],
    unknowns: list[dict[str, object]],
    renames: Mapping[str, str],
    asserted: bool,
) -> None:
    for symbol, new_name in sorted(renames.items()):
        matches: list[
            tuple[
                Mapping[str, object],
                Mapping[str, object],
                Mapping[str, object],
            ]
        ] = []
        for constraint in ordered_checks:
            raw_findings = constraint.get("findings")
            if not isinstance(raw_findings, list):
                continue
            findings = _coalesce_findings(
                [row for row in raw_findings if isinstance(row, Mapping)]
            )
            extras = [
                row
                for row in findings
                if isinstance(row, Mapping)
                and row.get("key") == symbol
                and row.get("comparison") in {"extra", "overlap"}
                and _finding_is_current(row)
            ]
            missing = [
                row
                for row in findings
                if isinstance(row, Mapping)
                and row.get("key") == new_name
                and row.get("comparison") == "missing"
                and _finding_is_current(row)
            ]
            if len(extras) == 1 and len(missing) == 1:
                matches.append((constraint, extras[0], missing[0]))
        if len(matches) != 1:
            raise ValueError(
                f"rename {symbol}={new_name} must match exactly one selected "
                "constraint containing one current extra/overlap source and one "
                "current missing target"
            )
        constraint, source_finding, target_finding = matches[0]
        constraint_id = constraint.get("id")
        assert isinstance(constraint_id, str)
        evidence = _evidence(source_finding)
        seed_paths = [
            row["path"]
            for row in evidence
            if isinstance(row.get("path"), str) and row["path"]
        ]
        operation, reasons = _projected_rename_operation(
            map_document=map_document,
            root=root,
            symbol=symbol,
            new_name=new_name,
            seed_paths=seed_paths,
        )
        item_id = _operation_id(constraint_id, None, operation)
        policy_result = policy_results.get(constraint_id)
        origins = [
            _origin(constraint_id, source_finding, policy_result),
            _origin(constraint_id, target_finding, policy_result),
        ]
        origin_by_identity = {
            _canonical_bytes(origin): origin for origin in origins
        }
        combined_evidence = _evidence(source_finding) + _evidence(target_finding)
        evidence_by_identity = {
            _canonical_bytes(row): row for row in combined_evidence
        }
        unique_reasons = list(dict.fromkeys(reasons))
        if not operation["sites"]:
            unique_reasons.append(
                f"No exact source sites were available for renaming {symbol}."
            )
        if not asserted:
            unique_reasons.append(
                f"Rename intent {symbol}={new_name} is inferred from one extra "
                "and one missing symbol; review it with --rename before Act."
            )
        unknown_rows = [
            {
                "id": _unknown_id(item_id, reason),
                "statement": reason,
                "item_id": item_id,
                "constraint_id": constraint_id,
            }
            for reason in unique_reasons
        ]
        replaced_fingerprints = {
            source_finding.get("fingerprint"),
            target_finding.get("fingerprint"),
        }
        removed_ids = {
            str(item["id"])
            for item in items
            if any(
                isinstance(origin, Mapping)
                and origin.get("issue_fingerprint") in replaced_fingerprints
                for origin in item.get("origins", [])
            )
        }
        items[:] = [item for item in items if item.get("id") not in removed_ids]
        unknowns[:] = [
            row for row in unknowns if row.get("item_id") not in removed_ids
        ]
        items.append(
            {
                "id": item_id,
                "statement": (
                    (
                        f"Rename {symbol} to {new_name} across its complete "
                        f"architecture evidence closure for {constraint_id}."
                    )
                    if asserted
                    else (
                        f"Review the inferred rename from {symbol} to {new_name} "
                        f"for {constraint_id}."
                    )
                ),
                "provenance": "asserted" if asserted else "derived",
                "origins": [
                    origin_by_identity[key] for key in sorted(origin_by_identity)
                ],
                "evidence": [
                    evidence_by_identity[key] for key in sorted(evidence_by_identity)
                ],
                "depends_on": [],
                "executable": not unique_reasons,
                "non_executable_reasons": unique_reasons,
                "operation": operation,
                "acceptance": {"constraints": [constraint_id]},
                "unknowns": [row["id"] for row in unknown_rows],
            }
        )
        unknowns.extend(unknown_rows)


def _inferred_rename_directives(
    constraints: Sequence[Mapping[str, object]],
) -> dict[str, str]:
    candidates: list[tuple[str, str]] = []
    for constraint in constraints:
        raw_findings = constraint.get("findings")
        if not isinstance(raw_findings, list):
            continue
        findings = _coalesce_findings(
            [row for row in raw_findings if isinstance(row, Mapping)]
        )
        extras = [
            row
            for row in findings
            if isinstance(row, Mapping)
            and row.get("comparison") in {"extra", "overlap"}
            and isinstance(row.get("key"), str)
            and row["key"]
            and _finding_is_current(row)
        ]
        missing = [
            row
            for row in findings
            if isinstance(row, Mapping)
            and row.get("comparison") == "missing"
            and isinstance(row.get("key"), str)
            and row["key"]
            and _finding_is_current(row)
        ]
        if len(extras) == 1 and len(missing) == 1:
            candidates.append((str(extras[0]["key"]), str(missing[0]["key"])))
    counts: dict[str, int] = {}
    for symbol, _ in candidates:
        counts[symbol] = counts.get(symbol, 0) + 1
    return {
        symbol: new_name
        for symbol, new_name in candidates
        if counts[symbol] == 1
    }


def generate_plan(
    map_document: Mapping[str, object],
    verification_document: Mapping[str, object],
    constraint_ids: Sequence[str] | None,
    root: Path,
    renames: Mapping[str, str] | None = None,
) -> dict[str, object]:
    """Generate deterministic Plan items without inventing transformation inputs."""

    if map_document.get("artifact") != "map":
        raise ValueError("generate_plan requires a Map artifact")
    if verification_document.get("artifact") != "verification":
        raise ValueError("generate_plan requires a Verification artifact")
    if not isinstance(root, Path):
        root = Path(root)
    if root.is_symlink():
        raise ValueError("Plan project root must not be a symbolic link")
    if not root.is_dir():
        raise ValueError("Plan project root must be an existing directory")

    source = _source_identity(map_document, verification_document)
    constraints = verification_document.get("constraints")
    definitions = verification_document.get("operand_definitions")
    if not isinstance(constraints, list) or not isinstance(definitions, Mapping):
        raise ValueError("Verification is missing constraints or operand definitions")
    checks = {
        row["id"]: row
        for row in constraints
        if isinstance(row, Mapping) and isinstance(row.get("id"), str)
    }
    if len(checks) != len(constraints):
        raise ValueError(
            "Verification constraints must have unique non-empty string IDs"
        )
    invalid_ids = sorted(
        constraint_id
        for constraint_id in checks
        if not _ID.fullmatch(constraint_id)
    )
    if invalid_ids:
        raise ValueError(
            "Verification contains invalid constraint IDs: "
            + ", ".join(invalid_ids)
        )

    if constraint_ids is None:
        requested = list(checks)
    else:
        if isinstance(constraint_ids, (str, bytes)):
            raise ValueError("constraint_ids must be a sequence of IDs")
        requested = list(dict.fromkeys(constraint_ids))
        if any(not isinstance(item, str) or not item for item in requested):
            raise ValueError("constraint_ids must contain non-empty strings")
        missing = sorted(set(requested) - set(checks))
        if missing:
            raise ValueError(
                "Verification does not contain requested constraints: "
                + ", ".join(missing)
            )
    requested_set = set(requested)
    ordered_checks = [
        row
        for row in constraints
        if isinstance(row, Mapping) and row.get("id") in requested_set
    ]
    files, duplicate_paths = _map_files(map_document)
    policy_results = _policy_results(verification_document)
    if set(policy_results) != set(checks):
        raise ValueError(
            "Verification policy results do not match evaluated constraints"
        )
    for constraint_id, result in policy_results.items():
        digest = result.get("constraint_result_sha256")
        if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
            raise ValueError(
                "Verification policy has an invalid result digest for "
                f"{constraint_id}"
            )
    items: list[dict[str, object]] = []
    unknowns: list[dict[str, object]] = []
    relation_frontier_error = _destructive_relation_frontier(map_document)

    for constraint in ordered_checks:
        constraint_id = constraint["id"]
        assert isinstance(constraint_id, str)
        status = constraint.get("status")
        if status in ("pass", "waived", "not_applicable"):
            continue
        form, actual_definition = _constraint_form(constraint, definitions)
        if form == "forbidden_paths":
            for item, rows in _forbidden_path_items(
                root=root,
                map_document=map_document,
                verification_document=verification_document,
                files=files,
                duplicate_paths=duplicate_paths,
                constraint=constraint,
                policy_result=policy_results.get(constraint_id),
                relation_frontier_error=relation_frontier_error,
            ):
                items.append(item)
                unknowns.extend(rows)
            continue
        findings = constraint.get("findings")
        finding_rows = (
            _coalesce_findings(
                [row for row in findings if isinstance(row, Mapping)]
            )
            if isinstance(findings, list)
            else []
        )
        if not finding_rows:
            item, rows = _non_executable_item(
                form=form,
                constraint_id=constraint_id,
                policy_result=policy_results.get(constraint_id),
                finding=None,
                actual_definition=actual_definition,
            )
            items.append(item)
            unknowns.extend(rows)
            continue
        actual_symbol_operand: Mapping[str, object] | None = None
        symbol_operand_error: str | None = None
        if form == "removable_symbol_set":
            actual_symbol_operand, symbol_operand_error = _complete_symbol_operands(
                verification_document, constraint
            )
        for finding in finding_rows:
            if (
                form == "removable_symbol_set"
                and _finding_is_current(finding)
                and _removable_symbol_finding(constraint, finding)
            ):
                correlated: Mapping[str, object] | None = None
                correlation_error: str | None = None
                if actual_symbol_operand is not None:
                    correlated, correlation_error = _correlate_symbol_finding(
                        actual_symbol_operand, finding
                    )
                reason = symbol_operand_error or correlation_error
                if reason or correlated is None:
                    item, rows = _manual_item(
                        constraint_id=constraint_id,
                        policy_result=policy_results.get(constraint_id),
                        finding=finding,
                        statement=(
                            f"Remove symbol {finding.get('key', constraint_id)} "
                            f"to satisfy {constraint_id}."
                        ),
                        instructions=(
                            "Establish exhaustive current symbol operands and one "
                            "source-witnessed declaration before removing code."
                        ),
                        reasons=(reason or "Symbol evidence is not removable.",),
                        candidate_paths=_candidate_paths(
                            finding, actual_definition
                        ),
                    )
                else:
                    item, rows = _removable_symbol_item(
                        root=root,
                        map_document=map_document,
                        files=files,
                        duplicate_paths=duplicate_paths,
                        constraint_id=constraint_id,
                        policy_result=policy_results.get(constraint_id),
                        finding=correlated,
                        relation_frontier_error=relation_frontier_error,
                    )
            else:
                manual_form = (
                    "required_symbols"
                    if form == "removable_symbol_set"
                    and finding.get("comparison") == "missing"
                    else form
                )
                item, rows = _non_executable_item(
                    form=manual_form,
                    constraint_id=constraint_id,
                    policy_result=policy_results.get(constraint_id),
                    finding=finding,
                    actual_definition=actual_definition,
                )
                if not _finding_is_current(finding):
                    reason = (
                        "Finding evidence is waived, stale, inapplicable, or otherwise "
                        "not current executable evidence."
                    )
                    item["non_executable_reasons"].append(reason)
                    unknown_id = _unknown_id(item["id"], reason)
                    item["unknowns"].append(unknown_id)
                    rows.append(
                        {
                            "id": unknown_id,
                            "statement": reason,
                            "item_id": item["id"],
                            "constraint_id": constraint_id,
                        }
                    )
            items.append(item)
            unknowns.extend(rows)

    if renames:
        if any(
            not isinstance(symbol, str)
            or not symbol
            or not isinstance(new_name, str)
            or not new_name
            for symbol, new_name in renames.items()
        ):
            raise ValueError("renames must map non-empty symbol names")
        _apply_rename_directives(
            map_document=map_document,
            root=root,
            ordered_checks=ordered_checks,
            policy_results=policy_results,
            items=items,
            unknowns=unknowns,
            renames=renames,
            asserted=True,
        )
    else:
        inferred_renames = _inferred_rename_directives(ordered_checks)
        if inferred_renames:
            _apply_rename_directives(
                map_document=map_document,
                root=root,
                ordered_checks=ordered_checks,
                policy_results=policy_results,
                items=items,
                unknowns=unknowns,
                renames=inferred_renames,
                asserted=False,
            )

    targeted = {
        constraint_id
        for item in items
        for constraint_id in item["acceptance"]["constraints"]
    }
    preserved = [
        constraint_id for constraint_id in checks if constraint_id not in targeted
    ]
    plan = {
        "schema_version": 1,
        "artifact": "plan",
        "provenance": "asserted" if renames else "derived",
        "objective": (
            "Apply the reviewed symbol renames and satisfy the selected "
            "Verification constraints without regressing preserved constraints."
            if renames
            else "Satisfy the selected Verification constraints without regressing "
            "preserved constraints."
        ),
        "tool": {
            "name": "archbird",
            "version": __version__,
            "implementation_sha256": implementation_digest(),
        },
        "source": source,
        "items": items,
        "preserved_constraints": preserved,
        "unknowns": unknowns,
    }
    if (
        len(items) > MAX_COLLECTION_ITEMS
        or len(unknowns) > MAX_COLLECTION_ITEMS
        or len(preserved) > MAX_COLLECTION_ITEMS
    ):
        raise ValueError(
            f"generated Plan exceeds the {MAX_COLLECTION_ITEMS}-item "
            "collection limit"
        )
    for item in items:
        if len(str(item["statement"]).encode("utf-8")) > MAX_METADATA_BYTES:
            raise ValueError("generated Plan item statement exceeds its byte limit")
        operation = item["operation"]
        assert isinstance(operation, Mapping)
        for key in ("content", "before", "replacement"):
            value = operation.get(key)
            if (
                isinstance(value, str)
                and len(value.encode("utf-8")) > MAX_OPERATION_TEXT_BYTES
            ):
                raise ValueError(
                    f"generated Plan operation {key} exceeds its byte limit"
                )
    if len(_canonical_bytes(plan)) > MAX_PLAN_BYTES:
        raise ValueError(
            f"generated Plan exceeds the {MAX_PLAN_BYTES}-byte canonical limit"
        )
    return plan
