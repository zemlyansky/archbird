#!/usr/bin/env python3
"""Build and replay Archbird's external historical-change evaluation corpus.

The corpus, repository mirrors, checkouts, and observed run artifacts live under
the absolute directory named by ARCHBIRD_EVAL_ROOT.  This development harness
never embeds that machine-local path in a canonical artifact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Mapping, Optional, Sequence


SCHEMA_VERSION = 1
RUN_SCHEMA_VERSION = 3
COMPARISON_SCHEMA_VERSION = 3
ROOT_ENV = "ARCHBIRD_EVAL_ROOT"
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]*$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CASE_ARTIFACT = "archbird-evaluation-case"
CORPUS_ARTIFACT = "archbird-evaluation-corpus"
RUN_ARTIFACT = "archbird-evaluation-run"
COMPARISON_ARTIFACT = "archbird-evaluation-comparison"
RESERVATION_PLAN_ARTIFACT = "archbird-evaluation-reservation-plan"
RESERVATION_ARTIFACT = "archbird-evaluation-reservation"
RESERVATION_COMPARISON_ARTIFACT = "archbird-evaluation-reservation-comparison"
PROTOCOL_ARTIFACT = "archbird-evaluation-protocol"
STATE_ARTIFACT = "archbird-evaluation-state"
HELD_OUT_CLAIM_ARTIFACT = "archbird-evaluation-held-out-claim"
HELD_OUT_CLAIM_FILE = "held-out-opened.json"
PROVIDER_CACHE_MAX_BYTES = 1_073_741_824
CACHE_POLICY = {
    "provider_cache": {
        "initial_state": "empty",
        "max_bytes": PROVIDER_CACHE_MAX_BYTES,
        "mode": "isolated_per_case",
        "reuse": "before_and_after_revision",
    }
}
CACHE_ENVIRONMENT_KEYS = (
    "ARCHBIRD_CACHE_DIR",
    "ARCHBIRD_CACHE_MAX_BYTES",
)


class EvaluationError(RuntimeError):
    pass


def canonical(value: object) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(value)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_json(path: Path, value: object) -> None:
    atomic_write(path, canonical(value) + b"\n")


def read_json(path: Path, label: str) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise EvaluationError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise EvaluationError(f"{label} must be a JSON object: {path}")
    return value


def root_from_environment() -> Path:
    raw = os.environ.get(ROOT_ENV)
    if not raw:
        raise EvaluationError(f"{ROOT_ENV} must name the external evaluation root")
    root = Path(raw).expanduser()
    if not root.is_absolute():
        raise EvaluationError(f"{ROOT_ENV} must be an absolute path")
    return root.resolve()


def safe_id(value: object, label: str) -> str:
    if not isinstance(value, str) or not ID_RE.fullmatch(value):
        raise EvaluationError(f"{label} must be a stable identifier")
    return value


def safe_relative(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise EvaluationError(f"{label} must be a non-empty repository path")
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or "\\" in value
        or "//" in value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise EvaluationError(f"{label} is not a safe repository path: {value!r}")
    return path.as_posix()


def exact_keys(value: Mapping[str, Any], required: set[str], optional: set[str], label: str) -> None:
    keys = set(value)
    missing = required - keys
    extra = keys - required - optional
    if missing or extra:
        raise EvaluationError(
            f"{label} keys differ: missing={sorted(missing)} extra={sorted(extra)}"
        )


def sorted_unique_strings(
    value: object, label: str, *, paths: bool = False
) -> list[str]:
    if not isinstance(value, list):
        raise EvaluationError(f"{label} must be an array")
    result = [safe_relative(item, label) if paths else str(item) for item in value]
    if any(not item for item in result) or result != sorted(set(result), key=str.encode):
        raise EvaluationError(f"{label} must be sorted and unique")
    return result


def validate_case(value: Mapping[str, Any], source: Path | None = None) -> Mapping[str, Any]:
    where = str(source) if source else "evaluation case"
    exact_keys(
        value,
        {
            "schema_version",
            "artifact",
            "provenance",
            "id",
            "split",
            "repository",
            "task",
            "analysis",
            "ground_truth",
            "review",
        },
        set(),
        where,
    )
    if value["schema_version"] != SCHEMA_VERSION or value["artifact"] != CASE_ARTIFACT:
        raise EvaluationError(f"{where} has unsupported identity")
    if value["provenance"] != "asserted":
        raise EvaluationError(f"{where}.provenance must be asserted")
    safe_id(value["id"], f"{where}.id")
    if value["split"] not in {"development", "validation", "held_out"}:
        raise EvaluationError(f"{where}.split is invalid")

    repository = value["repository"]
    if not isinstance(repository, dict):
        raise EvaluationError(f"{where}.repository must be an object")
    exact_keys(
        repository,
        {"id", "url", "before_revision", "after_revision"},
        set(),
        f"{where}.repository",
    )
    safe_id(repository["id"], f"{where}.repository.id")
    if not isinstance(repository["url"], str) or not repository["url"]:
        raise EvaluationError(f"{where}.repository.url must be non-empty")
    for key in ("before_revision", "after_revision"):
        if not isinstance(repository[key], str) or not REVISION_RE.fullmatch(repository[key]):
            raise EvaluationError(f"{where}.repository.{key} must be a full Git revision")
    if repository["before_revision"] == repository["after_revision"]:
        raise EvaluationError(f"{where} before and after revisions must differ")

    task = value["task"]
    if not isinstance(task, dict):
        raise EvaluationError(f"{where}.task must be an object")
    exact_keys(task, {"title", "description", "source"}, set(), f"{where}.task")
    for key in ("title", "description", "source"):
        if not isinstance(task[key], str) or not task[key]:
            raise EvaluationError(f"{where}.task.{key} must be non-empty")

    analysis = value["analysis"]
    if not isinstance(analysis, dict):
        raise EvaluationError(f"{where}.analysis must be an object")
    exact_keys(
        analysis,
        {"track", "seed_rationale"},
        {
            "config",
            "depth",
            "direction",
            "level",
            "max_depth",
            "max_paths",
            "relations",
            "selectors",
            "source",
            "target",
            "test_depth",
        },
        f"{where}.analysis",
    )
    if analysis["track"] not in {"exact_query", "exact_impact", "exact_path"}:
        raise EvaluationError(f"{where}.analysis.track is invalid")
    if not isinstance(analysis["seed_rationale"], str) or not analysis["seed_rationale"]:
        raise EvaluationError(f"{where}.analysis.seed_rationale must be non-empty")
    if analysis.get("config") is not None and not isinstance(analysis.get("config"), dict):
        raise EvaluationError(f"{where}.analysis.config must be an object or null")
    if analysis["track"] == "exact_path":
        exact_keys(
            analysis,
            {
                "track",
                "seed_rationale",
                "source",
                "target",
                "level",
                "relations",
                "direction",
                "max_depth",
                "max_paths",
            },
            {"config"},
            f"{where}.analysis",
        )
        for endpoint_name in ("source", "target"):
            endpoint = analysis[endpoint_name]
            if not isinstance(endpoint, dict):
                raise EvaluationError(
                    f"{where}.analysis.{endpoint_name} must be an object"
                )
            exact_keys(
                endpoint,
                {"kind", "value"},
                set(),
                f"{where}.analysis.{endpoint_name}",
            )
            kind = endpoint["kind"]
            if (
                not isinstance(kind, str)
                or not re.fullmatch(r"(?:any|[a-z][a-z0-9]*(?:-[a-z0-9]+)*)", kind)
            ):
                raise EvaluationError(
                    f"{where}.analysis.{endpoint_name}.kind is invalid"
                )
            if not isinstance(endpoint["value"], str) or not endpoint["value"]:
                raise EvaluationError(
                    f"{where}.analysis.{endpoint_name}.value is empty"
                )
        if analysis["level"] not in {"component", "file", "symbol"}:
            raise EvaluationError(f"{where}.analysis.level is invalid")
        relations = analysis["relations"]
        allowed_relations = {
            "bridges",
            "builds",
            "calls",
            "declarations",
            "imports",
            "packages",
            "references",
            "tests",
        }
        if (
            not isinstance(relations, list)
            or not relations
            or any(relation not in allowed_relations for relation in relations)
            or relations != sorted(set(relations), key=str.encode)
        ):
            raise EvaluationError(
                f"{where}.analysis.relations must be sorted, unique, and valid"
            )
        if analysis["level"] == "symbol" and any(
            relation not in {"calls", "references"} for relation in relations
        ):
            raise EvaluationError(
                f"{where}.analysis.relations contains a non-symbol relation"
            )
        if analysis["direction"] not in {"downstream", "upstream", "both"}:
            raise EvaluationError(f"{where}.analysis.direction is invalid")
        if (
            not isinstance(analysis["max_depth"], int)
            or isinstance(analysis["max_depth"], bool)
            or not 0 <= analysis["max_depth"] <= 64
        ):
            raise EvaluationError(f"{where}.analysis.max_depth is invalid")
        if (
            not isinstance(analysis["max_paths"], int)
            or isinstance(analysis["max_paths"], bool)
            or not 1 <= analysis["max_paths"] <= 100
        ):
            raise EvaluationError(f"{where}.analysis.max_paths is invalid")
    else:
        exact_keys(
            analysis,
            {"track", "selectors", "seed_rationale", "depth", "test_depth"},
            {"config"},
            f"{where}.analysis",
        )
        selectors = analysis["selectors"]
        if not isinstance(selectors, list) or not selectors:
            raise EvaluationError(f"{where}.analysis.selectors must be non-empty")
        selector_keys: list[tuple[str, str]] = []
        for index, selector in enumerate(selectors):
            if not isinstance(selector, dict):
                raise EvaluationError(f"{where}.analysis.selectors[{index}] must be an object")
            exact_keys(
                selector,
                {"kind", "value"},
                set(),
                f"{where}.analysis.selectors[{index}]",
            )
            if selector["kind"] not in {"path", "symbol", "test"}:
                raise EvaluationError(f"{where}.analysis.selectors[{index}].kind is invalid")
            if not isinstance(selector["value"], str) or not selector["value"]:
                raise EvaluationError(f"{where}.analysis.selectors[{index}].value is empty")
            selector_keys.append((selector["kind"], selector["value"]))
        if selector_keys != sorted(set(selector_keys), key=lambda item: (item[0].encode(), item[1].encode())):
            raise EvaluationError(f"{where}.analysis.selectors must be sorted and unique")
        for key in ("depth", "test_depth"):
            if not isinstance(analysis[key], int) or isinstance(analysis[key], bool) or not 0 <= analysis[key] <= 16:
                raise EvaluationError(f"{where}.analysis.{key} must be an integer from 0 to 16")

    truth = value["ground_truth"]
    if not isinstance(truth, dict):
        raise EvaluationError(f"{where}.ground_truth must be an object")
    exact_keys(
        truth,
        {
            "relevant_files",
            "relevant_symbols",
            "relevant_tests",
            "architecture_obligations",
            "notes",
        },
        {"introduced_tests", "path"},
        f"{where}.ground_truth",
    )
    sorted_unique_strings(truth["relevant_files"], f"{where}.ground_truth.relevant_files", paths=True)
    for key in ("relevant_symbols", "relevant_tests", "architecture_obligations"):
        sorted_unique_strings(truth[key], f"{where}.ground_truth.{key}")
    if "introduced_tests" in truth:
        sorted_unique_strings(
            truth["introduced_tests"], f"{where}.ground_truth.introduced_tests"
        )
    if not isinstance(truth["notes"], str):
        raise EvaluationError(f"{where}.ground_truth.notes must be a string")
    path_truth = truth.get("path")
    if analysis["track"] == "exact_path" and path_truth is None:
        raise EvaluationError(f"{where}.ground_truth.path is required")
    if analysis["track"] != "exact_path" and path_truth is not None:
        raise EvaluationError(
            f"{where}.ground_truth.path requires the exact_path track"
        )
    if path_truth is not None:
        if not isinstance(path_truth, dict):
            raise EvaluationError(f"{where}.ground_truth.path must be an object")
        exact_keys(
            path_truth,
            {"outcome", "reason", "shortest_length", "path_state", "nodes"},
            set(),
            f"{where}.ground_truth.path",
        )
        if path_truth["outcome"] not in {"found", "absent", "unknown"}:
            raise EvaluationError(
                f"{where}.ground_truth.path.outcome is invalid"
            )
        if path_truth["reason"] not in {
            "witnesses",
            "path-limit",
            "candidate-witnesses",
            "endpoint-unresolved",
            "graph-incomplete",
            "depth-frontier",
            "exhaustive",
        }:
            raise EvaluationError(
                f"{where}.ground_truth.path.reason is invalid"
            )
        shortest = path_truth["shortest_length"]
        if (
            shortest is not None
            and (
                not isinstance(shortest, int)
                or isinstance(shortest, bool)
                or not 0 <= shortest <= 64
            )
        ):
            raise EvaluationError(
                f"{where}.ground_truth.path.shortest_length is invalid"
            )
        if path_truth["path_state"] not in {None, "current", "unknown"}:
            raise EvaluationError(
                f"{where}.ground_truth.path.path_state is invalid"
            )
        nodes = path_truth["nodes"]
        if (
            not isinstance(nodes, list)
            or any(
                not isinstance(path, list)
                or not path
                or any(not isinstance(node, str) or not node for node in path)
                for path in nodes
            )
        ):
            raise EvaluationError(f"{where}.ground_truth.path.nodes is invalid")
        if (
            bool(nodes) != (shortest is not None)
            or (nodes and any(len(path) != shortest + 1 for path in nodes))
            or (bool(nodes) != (path_truth["path_state"] is not None))
        ):
            raise EvaluationError(
                f"{where}.ground_truth.path witness ledger is inconsistent"
            )

    review = value["review"]
    if not isinstance(review, dict):
        raise EvaluationError(f"{where}.review must be an object")
    exact_keys(
        review,
        {"status", "rationale"},
        {"evidence", "reviewer"},
        f"{where}.review",
    )
    if review["status"] not in {"candidate", "reviewed"}:
        raise EvaluationError(f"{where}.review.status is invalid")
    if not isinstance(review["rationale"], str) or not review["rationale"]:
        raise EvaluationError(f"{where}.review.rationale must be non-empty")
    if "reviewer" in review and (
        not isinstance(review["reviewer"], str) or not review["reviewer"]
    ):
        raise EvaluationError(f"{where}.review.reviewer must be non-empty")
    if "evidence" in review:
        sorted_unique_strings(review["evidence"], f"{where}.review.evidence")
    return value


def validate_protocol(
    value: Mapping[str, Any], source: Path | None = None
) -> Mapping[str, Any]:
    where = str(source) if source else "evaluation protocol"
    exact_keys(
        value,
        {
            "schema_version",
            "artifact",
            "provenance",
            "id",
            "case_selection",
            "analysis",
            "metrics",
            "split_policy",
        },
        set(),
        where,
    )
    if value["schema_version"] != SCHEMA_VERSION or value["artifact"] != PROTOCOL_ARTIFACT:
        raise EvaluationError(f"{where} has unsupported identity")
    if value["provenance"] != "asserted":
        raise EvaluationError(f"{where}.provenance must be asserted")
    safe_id(value["id"], f"{where}.id")

    selection = value["case_selection"]
    if not isinstance(selection, dict):
        raise EvaluationError(f"{where}.case_selection must be an object")
    exact_keys(
        selection,
        {
            "order",
            "single_parent",
            "max_changed_files",
            "require_product_source",
            "require_tests",
            "excluded_subject_prefixes",
        },
        set(),
        f"{where}.case_selection",
    )
    if selection["order"] != "reverse_chronological_first_eligible":
        raise EvaluationError(f"{where}.case_selection.order is unsupported")
    for key in ("single_parent", "require_product_source", "require_tests"):
        if not isinstance(selection[key], bool):
            raise EvaluationError(f"{where}.case_selection.{key} must be boolean")
    if (
        not isinstance(selection["max_changed_files"], int)
        or isinstance(selection["max_changed_files"], bool)
        or selection["max_changed_files"] < 1
    ):
        raise EvaluationError(
            f"{where}.case_selection.max_changed_files must be positive"
        )
    sorted_unique_strings(
        selection["excluded_subject_prefixes"],
        f"{where}.case_selection.excluded_subject_prefixes",
    )

    analysis = value["analysis"]
    if not isinstance(analysis, dict):
        raise EvaluationError(f"{where}.analysis must be an object")
    exact_keys(
        analysis,
        {
            "track",
            "selector_policy",
            "depth",
            "test_depth",
            "configuration",
            "issue_query_source",
            "issue_search_limit",
        },
        set(),
        f"{where}.analysis",
    )
    if analysis["track"] != "seeded_routing":
        raise EvaluationError(f"{where}.analysis.track is unsupported")
    if analysis["selector_policy"] != "task_explicit_identity":
        raise EvaluationError(f"{where}.analysis.selector_policy is unsupported")
    if analysis["configuration"] != "zero_config":
        raise EvaluationError(f"{where}.analysis.configuration is unsupported")
    if analysis["issue_query_source"] != "task_title":
        raise EvaluationError(
            f"{where}.analysis.issue_query_source is unsupported"
        )
    if (
        not isinstance(analysis["issue_search_limit"], int)
        or isinstance(analysis["issue_search_limit"], bool)
        or not 1 <= analysis["issue_search_limit"] <= 100
    ):
        raise EvaluationError(
            f"{where}.analysis.issue_search_limit must be from 1 to 100"
        )
    for key in ("depth", "test_depth"):
        if (
            not isinstance(analysis[key], int)
            or isinstance(analysis[key], bool)
            or not 0 <= analysis[key] <= 16
        ):
            raise EvaluationError(f"{where}.analysis.{key} is invalid")

    metrics = value["metrics"]
    if not isinstance(metrics, dict):
        raise EvaluationError(f"{where}.metrics must be an object")
    exact_keys(
        metrics,
        {
            "retrieval",
            "context",
            "performance",
            "split_aggregation",
            "introduced_tests_role",
        },
        {"line_budgets"},
        f"{where}.metrics",
    )
    sorted_unique_strings(metrics["retrieval"], f"{where}.metrics.retrieval")
    sorted_unique_strings(metrics["context"], f"{where}.metrics.context")
    if metrics["performance"] != "observational_single_sample":
        raise EvaluationError(f"{where}.metrics.performance is unsupported")
    if metrics["split_aggregation"] != "separate":
        raise EvaluationError(f"{where}.metrics.split_aggregation is unsupported")
    if metrics["introduced_tests_role"] != "act_only":
        raise EvaluationError(f"{where}.metrics.introduced_tests_role is unsupported")
    line_budgets = metrics.get("line_budgets", [200, 500, 1000, 2000])
    if (
        not isinstance(line_budgets, list)
        or not line_budgets
        or any(
            not isinstance(value, int)
            or isinstance(value, bool)
            or value <= 0
            for value in line_budgets
        )
        or line_budgets != sorted(set(line_budgets))
    ):
        raise EvaluationError(
            f"{where}.metrics.line_budgets must be sorted unique positive integers"
        )

    split_policy = value["split_policy"]
    if not isinstance(split_policy, dict):
        raise EvaluationError(f"{where}.split_policy must be an object")
    exact_keys(
        split_policy,
        {
            "development",
            "validation",
            "held_out",
            "held_out_open_once",
            "tuning_after_held_out_open",
        },
        set(),
        f"{where}.split_policy",
    )
    if split_policy["development"] != "tuning_allowed":
        raise EvaluationError(f"{where}.split_policy.development is unsupported")
    if split_policy["validation"] != "open_after_reservation":
        raise EvaluationError(f"{where}.split_policy.validation is unsupported")
    if split_policy["held_out"] != "sealed_until_protocol_frozen":
        raise EvaluationError(f"{where}.split_policy.held_out is unsupported")
    if split_policy["held_out_open_once"] is not True:
        raise EvaluationError(f"{where}.split_policy.held_out_open_once must be true")
    if split_policy["tuning_after_held_out_open"] is not False:
        raise EvaluationError(
            f"{where}.split_policy.tuning_after_held_out_open must be false"
        )
    return value


def evaluation_protocol(root: Path) -> tuple[Path, Mapping[str, Any], bytes]:
    path = root / "authoring/protocol.json"
    if not path.is_file():
        raise EvaluationError(f"missing asserted evaluation protocol: {path}")
    value = validate_protocol(read_json(path, "evaluation protocol"), path)
    encoded = canonical(value) + b"\n"
    return path, value, encoded


def validate_reservation_plan(
    value: Mapping[str, Any], source: Path | None = None
) -> Mapping[str, Any]:
    where = str(source) if source else "repository reservation plan"
    exact_keys(
        value,
        {
            "schema_version",
            "artifact",
            "provenance",
            "selection_rule",
            "repositories",
        },
        set(),
        where,
    )
    if (
        value["schema_version"] != SCHEMA_VERSION
        or value["artifact"] != RESERVATION_PLAN_ARTIFACT
    ):
        raise EvaluationError(f"{where} has unsupported identity")
    if value["provenance"] != "asserted":
        raise EvaluationError(f"{where}.provenance must be asserted")
    if not isinstance(value["selection_rule"], str) or not value["selection_rule"]:
        raise EvaluationError(f"{where}.selection_rule must be non-empty")
    repositories = value["repositories"]
    if not isinstance(repositories, list) or not repositories:
        raise EvaluationError(f"{where}.repositories must be non-empty")
    ids: list[str] = []
    urls: set[str] = set()
    for index, repository in enumerate(repositories):
        label = f"{where}.repositories[{index}]"
        if not isinstance(repository, dict):
            raise EvaluationError(f"{label} must be an object")
        exact_keys(
            repository,
            {
                "id",
                "url",
                "primary_language",
                "split",
                "inspection_policy",
                "rationale",
            },
            set(),
            label,
        )
        repository_id = safe_id(repository["id"], f"{label}.id")
        ids.append(repository_id)
        url = repository["url"]
        if not isinstance(url, str) or not url:
            raise EvaluationError(f"{label}.url must be non-empty")
        if url in urls:
            raise EvaluationError(f"{where} has duplicate repository URL: {url}")
        urls.add(url)
        if not isinstance(repository["primary_language"], str) or not repository["primary_language"]:
            raise EvaluationError(f"{label}.primary_language must be non-empty")
        if repository["split"] not in {"validation", "held_out"}:
            raise EvaluationError(f"{label}.split must be validation or held_out")
        if repository["inspection_policy"] not in {
            "open_after_reservation",
            "sealed_until_scoring",
        }:
            raise EvaluationError(f"{label}.inspection_policy is invalid")
        expected_policy = (
            "open_after_reservation"
            if repository["split"] == "validation"
            else "sealed_until_scoring"
        )
        if repository["inspection_policy"] != expected_policy:
            raise EvaluationError(
                f"{label}.inspection_policy must be {expected_policy} for its split"
            )
        if not isinstance(repository["rationale"], str) or not repository["rationale"]:
            raise EvaluationError(f"{label}.rationale must be non-empty")
    if ids != sorted(set(ids), key=str.encode):
        raise EvaluationError(f"{where}.repositories must be sorted and unique by id")
    return value


def initial_state() -> Mapping[str, Any]:
    return {
        "artifact": STATE_ARTIFACT,
        "current_corpus_sha256": None,
        "current_corpus_comparison": None,
        "current_reservation_sha256": None,
        "current_reservation_comparison": None,
        "current_run_sha256": None,
        "current_run_comparison": None,
        "generation": 0,
        "held_out_opened_corpus_sha256": None,
        "held_out_opened_label": None,
        "held_out_run_sha256": None,
        "previous_corpus_sha256": None,
        "previous_reservation_sha256": None,
        "previous_run_sha256": None,
        "schema_version": SCHEMA_VERSION,
    }


def read_state(root: Path) -> Mapping[str, Any]:
    state = dict(read_json(root / "state.json", "evaluation state"))
    if state.get("artifact") != STATE_ARTIFACT or state.get("schema_version") != SCHEMA_VERSION:
        raise EvaluationError("evaluation state has unsupported identity")
    for key in (
        "held_out_opened_corpus_sha256",
        "held_out_opened_label",
        "held_out_run_sha256",
    ):
        state.setdefault(key, None)
    opened = state["held_out_opened_corpus_sha256"]
    label = state["held_out_opened_label"]
    run = state["held_out_run_sha256"]
    if opened is not None and (
        not isinstance(opened, str) or not SHA256_RE.fullmatch(opened)
    ):
        raise EvaluationError("evaluation state held-out corpus digest is invalid")
    if label is not None and (not isinstance(label, str) or not label):
        raise EvaluationError("evaluation state held-out label is invalid")
    if run is not None and (not isinstance(run, str) or not SHA256_RE.fullmatch(run)):
        raise EvaluationError("evaluation state held-out run digest is invalid")
    if opened is None and (label is not None or run is not None):
        raise EvaluationError("evaluation state has incomplete held-out opening evidence")
    return state


def read_held_out_claim(root: Path) -> Mapping[str, Any] | None:
    path = root / HELD_OUT_CLAIM_FILE
    if not path.exists():
        return None
    claim = read_json(path, "held-out opening claim")
    exact_keys(
        claim,
        {
            "artifact",
            "corpus_sha256",
            "evaluator",
            "label",
            "provenance",
            "schema_version",
        },
        set(),
        "held-out opening claim",
    )
    if (
        claim["artifact"] != HELD_OUT_CLAIM_ARTIFACT
        or claim["schema_version"] != SCHEMA_VERSION
        or claim["provenance"] != "observed"
    ):
        raise EvaluationError("held-out opening claim has unsupported identity")
    if not isinstance(claim["corpus_sha256"], str) or not SHA256_RE.fullmatch(
        claim["corpus_sha256"]
    ):
        raise EvaluationError("held-out opening claim corpus digest is invalid")
    if not isinstance(claim["label"], str) or not claim["label"]:
        raise EvaluationError("held-out opening claim label is invalid")
    evaluator = claim["evaluator"]
    if (
        not isinstance(evaluator, dict)
        or set(evaluator) != {"implementation_sha256"}
        or not isinstance(evaluator["implementation_sha256"], str)
        or not SHA256_RE.fullmatch(evaluator["implementation_sha256"])
    ):
        raise EvaluationError("held-out opening claim evaluator is invalid")
    return claim


def reject_after_held_out_open(root: Path, operation: str) -> None:
    claim = read_held_out_claim(root)
    if claim is not None:
        raise EvaluationError(
            "held-out set was already opened for corpus "
            f"{claim['corpus_sha256']} by run label {claim['label']!r}; "
            f"{operation} is prohibited by the frozen protocol"
        )


def claim_held_out_opening(root: Path, corpus_sha256: str, label: str) -> None:
    claim = {
        "artifact": HELD_OUT_CLAIM_ARTIFACT,
        "corpus_sha256": corpus_sha256,
        "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
        "label": label,
        "provenance": "observed",
        "schema_version": SCHEMA_VERSION,
    }
    path = root / HELD_OUT_CLAIM_FILE
    encoded = canonical(claim) + b"\n"
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        reject_after_held_out_open(root, "another evaluation run")
        raise AssertionError("unreachable")
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise
    state = dict(read_state(root))
    state.update(
        {
            "generation": int(state["generation"]) + 1,
            "held_out_opened_corpus_sha256": corpus_sha256,
            "held_out_opened_label": label,
        }
    )
    write_json(root / "state.json", state)


def initialize(root: Path) -> None:
    for relative in (
        "authoring/cases",
        "comparisons/corpora",
        "comparisons/reservations",
        "comparisons/runs",
        "corpora",
        "repositories",
        "reservations",
        "runs",
        "work",
    ):
        (root / relative).mkdir(parents=True, exist_ok=True)
    state_path = root / "state.json"
    if not state_path.exists():
        write_json(state_path, initial_state())
    else:
        read_state(root)


def authoring_cases(root: Path) -> list[tuple[Path, Mapping[str, Any]]]:
    paths = sorted((root / "authoring/cases").glob("*.json"), key=lambda path: path.name.encode())
    if not paths:
        raise EvaluationError("authoring/cases contains no evaluation cases")
    result = []
    ids = set()
    for path in paths:
        value = validate_case(read_json(path, "evaluation case"), path)
        case_id = str(value["id"])
        if path.name != f"{case_id}.json":
            raise EvaluationError(f"case filename must match id: {path}")
        if case_id in ids:
            raise EvaluationError(f"duplicate evaluation case id: {case_id}")
        ids.add(case_id)
        result.append((path, value))
    return result


def reservation_plan(root: Path) -> tuple[Path, Mapping[str, Any]]:
    path = root / "authoring/reservations.json"
    if not path.is_file():
        raise EvaluationError("authoring/reservations.json is missing")
    return path, validate_reservation_plan(
        read_json(path, "repository reservation plan"), path
    )


def repository_path(root: Path, repository_id: str) -> Path:
    return root / "repositories" / f"{repository_id}.git"


def git(repository: Path, *arguments: str, capture: bool = True) -> str:
    command = ["git", f"--git-dir={repository}", *arguments]
    try:
        result = subprocess.run(
            command,
            check=True,
            capture_output=capture,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise EvaluationError(f"Git command failed: {' '.join(command)}: {detail}") from error
    return result.stdout.strip() if capture else ""


def has_revision(repository: Path, revision: str) -> bool:
    result = subprocess.run(
        ["git", f"--git-dir={repository}", "cat-file", "-e", f"{revision}^{{commit}}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def sync_repositories(root: Path, fetch: bool, reservations: str) -> None:
    repositories: dict[str, tuple[str, set[str]]] = {}
    for _, case in authoring_cases(root):
        value = case["repository"]
        repository_id = str(value["id"])
        url = str(value["url"])
        revisions = {str(value["before_revision"]), str(value["after_revision"])}
        if repository_id in repositories and repositories[repository_id][0] != url:
            raise EvaluationError(f"repository {repository_id} has conflicting URLs")
        repositories.setdefault(repository_id, (url, set()))[1].update(revisions)
    if reservations != "none":
        _, reservation, _ = load_reservation(root)
        for value in reservation["repositories"]:
            if reservations == "validation" and value["split"] != "validation":
                continue
            repository_id = str(value["id"])
            url = str(value["url"])
            revision = str(value["head_revision"])
            if repository_id in repositories and repositories[repository_id][0] != url:
                raise EvaluationError(f"repository {repository_id} has conflicting URLs")
            repositories.setdefault(repository_id, (url, set()))[1].add(revision)
    for repository_id in sorted(repositories, key=str.encode):
        url, revisions = repositories[repository_id]
        destination = repository_path(root, repository_id)
        if not destination.exists():
            subprocess.run(
                ["git", "clone", "--bare", "--filter=blob:none", url, str(destination)],
                check=True,
            )
        actual_url = git(destination, "remote", "get-url", "origin")
        if actual_url != url:
            raise EvaluationError(
                f"repository {repository_id} origin differs: {actual_url!r} != {url!r}"
            )
        missing = [revision for revision in sorted(revisions) if not has_revision(destination, revision)]
        if missing and not fetch:
            raise EvaluationError(
                f"repository {repository_id} is missing revisions; rerun sync --fetch: {missing}"
            )
        for revision in missing:
            subprocess.run(
                [
                    "git",
                    f"--git-dir={destination}",
                    "fetch",
                    "--filter=blob:none",
                    "--no-tags",
                    "origin",
                    revision,
                ],
                check=True,
            )
        for revision in revisions:
            if not has_revision(destination, revision):
                raise EvaluationError(f"repository {repository_id} lacks revision {revision}")
        print(f"evaluation repository ready: {repository_id} ({len(revisions)} revisions)")


def diff_files(repository: Path, before: str, after: str) -> list[str]:
    raw = subprocess.run(
        [
            "git",
            f"--git-dir={repository}",
            "diff",
            "--name-only",
            "-z",
            "--find-renames",
            before,
            after,
            "--",
        ],
        check=True,
        capture_output=True,
    ).stdout
    values = [safe_relative(item.decode("utf-8"), "Git diff path") for item in raw.split(b"\0") if item]
    return sorted(set(values), key=str.encode)


def corpus_document(
    root: Path,
) -> tuple[Mapping[str, Any], dict[str, bytes], bytes]:
    _, _, protocol_bytes = evaluation_protocol(root)
    entries = []
    case_bytes: dict[str, bytes] = {}
    for path, case in authoring_cases(root):
        repository = case["repository"]
        mirror = repository_path(root, str(repository["id"]))
        if not mirror.is_dir():
            raise EvaluationError(f"repository is not synchronized: {repository['id']}")
        for key in ("before_revision", "after_revision"):
            if not has_revision(mirror, str(repository[key])):
                raise EvaluationError(f"repository {repository['id']} lacks {repository[key]}")
        encoded = canonical(case) + b"\n"
        case_sha256 = sha256_bytes(encoded)
        changed = diff_files(
            mirror,
            str(repository["before_revision"]),
            str(repository["after_revision"]),
        )
        entries.append(
            {
                "case_sha256": case_sha256,
                "diff_files": changed,
                "id": case["id"],
                "repository": repository["id"],
                "review_status": case["review"]["status"],
                "split": case["split"],
            }
        )
        case_bytes[str(case["id"])] = encoded
    document = {
        "artifact": CORPUS_ARTIFACT,
        "cases": entries,
        "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
        "protocol_sha256": sha256_bytes(protocol_bytes),
        "provenance": "derived",
        "schema_version": SCHEMA_VERSION,
    }
    return document, case_bytes, protocol_bytes


def compare_corpora(before: Mapping[str, Any], after: Mapping[str, Any], before_sha: str, after_sha: str) -> Mapping[str, Any]:
    old = {str(item["id"]): item for item in before["cases"]}
    new = {str(item["id"]): item for item in after["cases"]}
    return {
        "added": sorted(set(new) - set(old), key=str.encode),
        "after_corpus_sha256": after_sha,
        "artifact": "archbird-evaluation-corpus-comparison",
        "before_corpus_sha256": before_sha,
        "changed": sorted(
            (case_id for case_id in set(old) & set(new) if old[case_id] != new[case_id]),
            key=str.encode,
        ),
        "provenance": "derived",
        "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
        "removed": sorted(set(old) - set(new), key=str.encode),
        "schema_version": SCHEMA_VERSION,
        "unchanged": sorted(
            (case_id for case_id in set(old) & set(new) if old[case_id] == new[case_id]),
            key=str.encode,
        ),
    }


def publish_comparison(
    root: Path,
    family: str,
    before_sha256: str,
    after_sha256: str,
    value: Mapping[str, Any],
) -> str:
    encoded = canonical(value) + b"\n"
    digest = sha256_bytes(encoded)
    directory = root / "comparisons" / family / f"{before_sha256}--{after_sha256}"
    path = directory / f"{digest}.json"
    if path.exists():
        if sha256_file(path) != digest:
            raise EvaluationError(f"existing comparison is corrupt: {path}")
    else:
        atomic_write(path, encoded)
    return path.relative_to(root).as_posix()


def resolve_remote_head(url: str) -> tuple[str, str]:
    environment = os.environ.copy()
    environment["GIT_TERMINAL_PROMPT"] = "0"
    try:
        result = subprocess.run(
            ["git", "ls-remote", "--symref", "--exit-code", url, "HEAD"],
            check=True,
            capture_output=True,
            text=True,
            env=environment,
        )
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise EvaluationError(f"cannot resolve reservation HEAD for {url}: {detail}") from error
    branch: str | None = None
    revision: str | None = None
    for line in result.stdout.splitlines():
        if line.startswith("ref: ") and line.endswith("\tHEAD"):
            reference = line[5:-5]
            if not reference.startswith("refs/heads/"):
                raise EvaluationError(f"reservation HEAD is not a branch for {url}")
            branch = reference[len("refs/heads/") :]
        else:
            fields = line.split("\t")
            if len(fields) == 2 and fields[1] == "HEAD" and REVISION_RE.fullmatch(fields[0]):
                revision = fields[0]
    if not branch or revision is None:
        raise EvaluationError(f"reservation HEAD identity is incomplete for {url}")
    return branch, revision


def reservation_document(root: Path) -> Mapping[str, Any]:
    path, plan = reservation_plan(root)
    encoded_plan = canonical(plan) + b"\n"
    repositories = []
    for value in plan["repositories"]:
        branch, revision = resolve_remote_head(str(value["url"]))
        repositories.append(
            {
                **value,
                "default_branch": branch,
                "head_revision": revision,
            }
        )
    return {
        "artifact": RESERVATION_ARTIFACT,
        "authoring_sha256": sha256_bytes(encoded_plan),
        "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
        "provenance": "derived",
        "repositories": repositories,
        "schema_version": SCHEMA_VERSION,
        "selection_rule": plan["selection_rule"],
    }


def load_reservation(
    root: Path, reservation_sha256: str | None = None
) -> tuple[str, Mapping[str, Any], Path]:
    if reservation_sha256 is None:
        value = read_state(root).get("current_reservation_sha256")
        if not isinstance(value, str):
            raise EvaluationError("no frozen repository reservation")
        reservation_sha256 = value
    if not SHA256_RE.fullmatch(reservation_sha256):
        raise EvaluationError("reservation digest is invalid")
    directory = root / "reservations" / reservation_sha256
    path = directory / "reservation.json"
    if not path.is_file() or sha256_file(path) != reservation_sha256:
        raise EvaluationError(
            f"repository reservation is missing or corrupt: {reservation_sha256}"
        )
    return reservation_sha256, read_json(path, "repository reservation"), directory


def compare_reservations(
    before: Mapping[str, Any],
    after: Mapping[str, Any],
    before_sha256: str,
    after_sha256: str,
) -> Mapping[str, Any]:
    old = {str(item["id"]): item for item in before["repositories"]}
    new = {str(item["id"]): item for item in after["repositories"]}
    return {
        "added": sorted(set(new) - set(old), key=str.encode),
        "after_reservation_sha256": after_sha256,
        "artifact": RESERVATION_COMPARISON_ARTIFACT,
        "before_reservation_sha256": before_sha256,
        "changed": sorted(
            (key for key in set(old) & set(new) if old[key] != new[key]),
            key=str.encode,
        ),
        "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
        "provenance": "derived",
        "removed": sorted(set(old) - set(new), key=str.encode),
        "schema_version": SCHEMA_VERSION,
        "unchanged": sorted(
            (key for key in set(old) & set(new) if old[key] == new[key]),
            key=str.encode,
        ),
    }


def freeze_reservation(root: Path) -> str:
    reject_after_held_out_open(root, "repository reservation changes")
    state = dict(read_state(root))
    document = reservation_document(root)
    encoded = canonical(document) + b"\n"
    reservation_sha256 = sha256_bytes(encoded)
    destination = root / "reservations" / reservation_sha256
    if destination.exists():
        existing = destination / "reservation.json"
        if sha256_file(existing) != reservation_sha256:
            raise EvaluationError(f"existing reservation is corrupt: {destination}")
    else:
        temporary = Path(tempfile.mkdtemp(prefix=".reservation-", dir=root / "work"))
        try:
            atomic_write(temporary / "reservation.json", encoded)
            os.replace(temporary, destination)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    previous = state.get("current_reservation_sha256")
    if isinstance(previous, str) and previous != reservation_sha256:
        _, old, _ = load_reservation(root, previous)
        comparison = compare_reservations(old, document, previous, reservation_sha256)
        state["current_reservation_comparison"] = publish_comparison(
            root, "reservations", previous, reservation_sha256, comparison
        )
    state.update(
        {
            "current_reservation_sha256": reservation_sha256,
            "generation": int(state["generation"]) + 1,
            "previous_reservation_sha256": previous,
        }
    )
    write_json(root / "state.json", state)
    print(
        f"frozen repository reservation: {reservation_sha256} "
        f"({len(document['repositories'])} repositories)"
    )
    return reservation_sha256


def freeze_corpus(root: Path) -> str:
    reject_after_held_out_open(root, "corpus changes")
    state = dict(read_state(root))
    document, cases, protocol_bytes = corpus_document(root)
    encoded = canonical(document) + b"\n"
    corpus_sha256 = sha256_bytes(encoded)
    destination = root / "corpora" / corpus_sha256
    if destination.exists():
        existing = destination / "corpus.json"
        if sha256_file(existing) != corpus_sha256:
            raise EvaluationError(f"existing corpus is corrupt: {destination}")
    else:
        temporary = Path(tempfile.mkdtemp(prefix=".corpus-", dir=root / "work"))
        try:
            write_json(temporary / "corpus.json", document)
            atomic_write(temporary / "protocol.json", protocol_bytes)
            for case_id, value in cases.items():
                atomic_write(temporary / "cases" / f"{case_id}.json", value)
            os.replace(temporary, destination)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    previous = state.get("current_corpus_sha256")
    if isinstance(previous, str) and previous != corpus_sha256:
        old = read_json(root / "corpora" / previous / "corpus.json", "previous corpus")
        comparison = compare_corpora(old, document, previous, corpus_sha256)
        state["current_corpus_comparison"] = publish_comparison(
            root, "corpora", previous, corpus_sha256, comparison
        )
    state.update(
        {
            "current_corpus_sha256": corpus_sha256,
            "generation": int(state["generation"]) + 1,
            "previous_corpus_sha256": previous,
        }
    )
    write_json(root / "state.json", state)
    print(f"frozen evaluation corpus: {corpus_sha256} ({len(document['cases'])} cases)")
    return corpus_sha256


def load_corpus(root: Path, corpus_sha256: str | None = None) -> tuple[str, Mapping[str, Any], Path]:
    if corpus_sha256 is None:
        value = read_state(root).get("current_corpus_sha256")
        if not isinstance(value, str):
            raise EvaluationError("no frozen evaluation corpus")
        corpus_sha256 = value
    if not SHA256_RE.fullmatch(corpus_sha256):
        raise EvaluationError("corpus digest is invalid")
    directory = root / "corpora" / corpus_sha256
    path = directory / "corpus.json"
    if not path.is_file() or sha256_file(path) != corpus_sha256:
        raise EvaluationError(f"frozen corpus is missing or corrupt: {corpus_sha256}")
    document = read_json(path, "frozen corpus")
    protocol_sha256 = document.get("protocol_sha256")
    protocol_path = directory / "protocol.json"
    if (
        not isinstance(protocol_sha256, str)
        or not SHA256_RE.fullmatch(protocol_sha256)
        or not protocol_path.is_file()
        or sha256_file(protocol_path) != protocol_sha256
    ):
        raise EvaluationError(f"frozen corpus protocol is missing or corrupt: {corpus_sha256}")
    validate_protocol(read_json(protocol_path, "frozen evaluation protocol"), protocol_path)
    return corpus_sha256, document, directory


def evaluated_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for key in CACHE_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    return environment


def run_process(command: Sequence[str], cwd: Path, stdout_path: Path, stderr_path: Path) -> Mapping[str, Any]:
    import time

    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic_ns()
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=evaluated_environment(),
            stdout=stdout,
            stderr=stderr,
        )
        max_rss_kib: int | None = None
        if hasattr(os, "wait4"):
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
            max_rss_kib = int(usage.ru_maxrss)
        else:
            process.wait()
    duration_ms = (time.monotonic_ns() - started) / 1_000_000
    return {
        "duration_ms": round(duration_ms, 3),
        "max_rss_kib": max_rss_kib,
        "returncode": process.returncode,
        "stderr_bytes": stderr_path.stat().st_size,
        "stderr_sha256": sha256_file(stderr_path),
        "stdout_bytes": stdout_path.stat().st_size,
        "stdout_sha256": sha256_file(stdout_path),
    }


def deduplicate(values: Iterable[str]) -> list[str]:
    seen = set()
    result = []
    for value in values:
        if value not in seen:
            seen.add(value)
            result.append(value)
    return result


def rank_metrics(prefix: str, ranked: Sequence[str], relevant: Sequence[str]) -> Mapping[str, Any]:
    unique_ranked = deduplicate(ranked)
    truth = set(relevant)
    result: dict[str, Any] = {
        f"{prefix}_returned": len(unique_ranked),
        f"{prefix}_truth": len(truth),
    }
    if not truth:
        for suffix in ("recall_at_5", "recall_at_10", "recall_at_20", "recall_all", "mrr", "distractor_ratio"):
            result[f"{prefix}_{suffix}"] = None
        return result
    for limit in (5, 10, 20):
        result[f"{prefix}_recall_at_{limit}"] = len(truth.intersection(unique_ranked[:limit])) / len(truth)
    hits = truth.intersection(unique_ranked)
    result[f"{prefix}_recall_all"] = len(hits) / len(truth)
    first = next((index for index, value in enumerate(unique_ranked, 1) if value in truth), None)
    result[f"{prefix}_mrr"] = 0.0 if first is None else 1.0 / first
    result[f"{prefix}_distractor_ratio"] = (
        None if not unique_ranked else (len(unique_ranked) - len(hits)) / len(unique_ranked)
    )
    return result


def exact_path_metrics(
    document: Mapping[str, Any], expected: Mapping[str, Any]
) -> Mapping[str, Any]:
    paths = document.get("paths")
    search = document.get("search")
    if not isinstance(paths, list) or not isinstance(search, dict):
        raise EvaluationError("case Path artifact has an invalid result shape")
    actual_nodes: list[list[str]] = []
    actual_states: set[str] = set()
    for index, path in enumerate(paths):
        if not isinstance(path, dict):
            raise EvaluationError(f"case Path paths[{index}] must be an object")
        nodes = path.get("nodes")
        state = path.get("state")
        if (
            not isinstance(nodes, list)
            or any(not isinstance(node, str) for node in nodes)
            or state not in {"current", "unknown"}
        ):
            raise EvaluationError(
                f"case Path paths[{index}] has invalid nodes or state"
            )
        actual_nodes.append(nodes)
        actual_states.add(state)
    expected_state = expected["path_state"]
    actual_state = (
        next(iter(actual_states))
        if len(actual_states) == 1
        else None
    )
    outcome_accuracy = float(document.get("outcome") == expected["outcome"])
    reason_accuracy = float(document.get("reason") == expected["reason"])
    shortest_accuracy = float(
        search.get("shortest_length") == expected["shortest_length"]
    )
    state_accuracy = float(actual_state == expected_state)
    witness_accuracy = float(actual_nodes == expected["nodes"])
    flattened_actual = [
        node for witness in actual_nodes for node in witness
    ]
    flattened_expected = [
        node for witness in expected["nodes"] for node in witness
    ]
    metrics: dict[str, Any] = {
        "path_accuracy": float(
            outcome_accuracy
            and reason_accuracy
            and shortest_accuracy
            and state_accuracy
            and witness_accuracy
        ),
        "path_outcome_accuracy": outcome_accuracy,
        "path_reason_accuracy": reason_accuracy,
        "path_shortest_accuracy": shortest_accuracy,
        "path_state_accuracy": state_accuracy,
        "path_witness_accuracy": witness_accuracy,
        "path_witnesses_returned": len(actual_nodes),
        "path_witnesses_truth": len(expected["nodes"]),
    }
    metrics.update(
        rank_metrics("path_node", flattened_actual, flattened_expected)
    )
    return metrics


def symbol_identity(value: object, *, inherited_path: str | None = None) -> str | None:
    if not isinstance(value, dict) or not isinstance(value.get("name"), str):
        return None
    path = value.get("path", inherited_path)
    if not isinstance(path, str):
        return None
    return f"{path}:{value['name']}"


def selector_truth(
    selectors: Sequence[Mapping[str, Any]], checkout_root: Path
) -> tuple[list[str], list[str]]:
    seed_files: list[str] = []
    target_symbols: list[str] = []
    for selector in selectors:
        kind = str(selector["kind"])
        value = str(selector["value"])
        path: str | None = None
        if kind == "path":
            path = value
        elif kind == "symbol" and ":" in value:
            path, _ = value.rsplit(":", 1)
            target_symbols.append(value)
        elif kind == "test" and "::" in value:
            path, _ = value.split("::", 1)
        if (
            path is not None
            and not any(character in path for character in "*?[")
            and (checkout_root / path).is_file()
        ):
            seed_files.append(path)
    return deduplicate(seed_files), deduplicate(target_symbols)


def extract_rankings(
    query: Mapping[str, Any],
) -> tuple[
    list[str],
    list[str],
    list[str],
    list[str],
    list[str],
    list[str],
    Mapping[str, int],
]:
    canonical_file_rows = [
        item
        for item in query.get("files", [])
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    ]
    canonical_test_rows = [
        item
        for item in query.get("test_matches", [])
        if isinstance(item, dict)
        and isinstance(item.get("path"), str)
        and isinstance(item.get("selector"), str)
    ]
    metadata = query.get("query")
    raw_context = metadata.get("context") if isinstance(metadata, dict) else None
    if raw_context is not None and not isinstance(raw_context, dict):
        raise EvaluationError("case Query context must be an object")
    context = raw_context if isinstance(raw_context, dict) else {}
    profile = context.get("profile", "change")
    profile_defaults: Mapping[str, tuple[int | None, str, str, int | None]] = {
        "exact": (0, "exclude", "exclude", 12),
        "change": (1, "expand", "collapse", 24),
        "architecture": (None, "expand", "collapse", 64),
        "audit": (None, "expand", "expand", None),
    }
    if profile not in profile_defaults:
        raise EvaluationError("case Query has an invalid context profile")
    max_distance, candidate_policy, conservative_policy, default_quota = (
        profile_defaults.get(str(profile), profile_defaults["change"])
    )
    if "max_seed_distance" in context:
        if (
            not isinstance(context["max_seed_distance"], int)
            or isinstance(context["max_seed_distance"], bool)
            or context["max_seed_distance"] < 0
        ):
            raise EvaluationError(
                "case Query context max_seed_distance is invalid"
            )
        max_distance = int(context["max_seed_distance"])
    for name in ("candidate", "conservative"):
        if name in context and context[name] not in {
            "collapse",
            "expand",
            "exclude",
        }:
            raise EvaluationError(f"case Query context {name} policy is invalid")
    if "candidate" in context:
        candidate_policy = str(context["candidate"])
    if "conservative" in context:
        conservative_policy = str(context["conservative"])
    quotas = context.get("quotas", {})
    offsets = context.get("offsets", {})
    if not isinstance(quotas, dict) or not isinstance(offsets, dict):
        raise EvaluationError("case Query context counts are invalid")

    def context_count(values: Mapping[str, Any], name: str, default: int) -> int:
        value = values.get(name, default)
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or value < 0
        ):
            raise EvaluationError(
                f"case Query context {name} count is invalid"
            )
        return int(value)

    eligible_file_rows = []
    for row in canonical_file_rows:
        distance = row.get("distance")
        if (
            not isinstance(distance, int)
            or isinstance(distance, bool)
            or distance < 0
        ):
            raise EvaluationError("case Query file distance is invalid")
        if max_distance is not None and distance > max_distance:
            break
        eligible_file_rows.append(row)
    file_offset = context_count(offsets, "files", 0)
    file_quota = context_count(
        quotas,
        "files",
        len(eligible_file_rows) if default_quota is None else default_quota,
    )
    presented_file_rows = eligible_file_rows[
        file_offset : file_offset + file_quota
    ]
    allowed_provenance = context.get("provenance")
    if not isinstance(allowed_provenance, list):
        allowed_provenance = None
    allowed_confidence = context.get("confidence")
    if not isinstance(allowed_confidence, list):
        allowed_confidence = None

    def test_presented(row: Mapping[str, Any]) -> bool:
        axes = (
            row.get("provenance"),
            row.get("confidence"),
            row.get("evidence_scope"),
            row.get("target_role"),
            row.get("target"),
        )
        if not any(value is not None for value in axes):
            return True
        if any(value is None for value in axes):
            raise EvaluationError(
                "case Query test route axes are partially specified"
            )
        provenance = row["provenance"]
        confidence = row["confidence"]
        if allowed_provenance is not None and provenance not in allowed_provenance:
            return False
        if allowed_confidence is not None:
            if confidence not in allowed_confidence:
                return False
        elif profile == "exact" and confidence != "exact":
            return False
        seed_distance = row.get("seed_distance")
        if (
            isinstance(seed_distance, int)
            and not isinstance(seed_distance, bool)
            and max_distance is not None
            and seed_distance > max_distance
        ):
            return False
        if confidence == "candidate" and candidate_policy != "expand":
            return False
        if confidence == "conservative" and conservative_policy != "expand":
            return False
        return True

    eligible_test_rows = [
        row for row in canonical_test_rows if test_presented(row)
    ]
    test_offset = context_count(offsets, "test_matches", 0)
    test_quota = context_count(
        quotas,
        "test_matches",
        len(eligible_test_rows) if default_quota is None else default_quota,
    )
    presented_test_rows = eligible_test_rows[
        test_offset : test_offset + test_quota
    ]
    files = [
        str(item["path"])
        for item in presented_file_rows
    ]
    seed_files = [
        str(item["path"])
        for item in canonical_file_rows
        if item.get("distance") == 0
    ]
    symbols = []
    canonical_symbols = 0
    for file in canonical_file_rows:
        canonical_symbols += sum(
            1
            for symbol in file.get("symbols", [])
            if symbol_identity(symbol, inherited_path=str(file["path"]))
            is not None
        )
    for file in presented_file_rows:
        for symbol in file.get("symbols", []):
            identity = symbol_identity(symbol, inherited_path=str(file["path"]))
            if identity is not None:
                symbols.append(identity)
    matched_symbols = [
        identity
        for value in query.get("matched_symbols", [])
        if (identity := symbol_identity(value)) is not None
    ]
    matched_symbol_files = [
        str(value["path"])
        for value in query.get("matched_symbols", [])
        if isinstance(value, dict) and isinstance(value.get("path"), str)
    ]
    tests = []
    test_files = []
    for test in presented_test_rows:
        test_files.append(str(test["path"]))
        tests.append(f"{test['path']}::{test['selector']}")
    context_files = deduplicate([*files, *matched_symbol_files, *test_files])
    return (
        deduplicate(files),
        context_files,
        deduplicate(seed_files),
        deduplicate(symbols),
        deduplicate(matched_symbols),
        deduplicate(tests),
        {
            "canonical_file_evidence_returned": len(canonical_file_rows),
            "canonical_symbol_evidence_returned": canonical_symbols,
            "canonical_test_evidence_returned": len(canonical_test_rows),
        },
    )


def extract_retrieval_rankings(
    query: Mapping[str, Any],
) -> tuple[list[str], list[str]]:
    query_metadata = query.get("query", {})
    retrieval = (
        query_metadata.get("retrieval", {})
        if isinstance(query_metadata, dict)
        else {}
    )
    hits = retrieval.get("hits", []) if isinstance(retrieval, dict) else []
    files: list[str] = []
    symbols: list[str] = []
    for hit in hits:
        if not isinstance(hit, dict) or not isinstance(hit.get("path"), str):
            continue
        path = str(hit["path"])
        files.append(path)
        if hit.get("kind") == "symbol" and isinstance(hit.get("name"), str):
            symbols.append(f"{path}:{hit['name']}")
    return deduplicate(files), deduplicate(symbols)


def extract_test_tiers(query: Mapping[str, Any]) -> Mapping[str, list[str]]:
    order = {
        "observed": 0,
        "asserted": 1,
        "direct": 2,
        "candidate": 3,
        "conservative": 4,
        "unresolved": 5,
    }
    limits = {
        "observed_asserted": 1,
        "direct": 2,
        "candidate": 3,
        "all": 5,
    }
    result: dict[str, list[str]] = {name: [] for name in limits}
    for row in query.get("test_matches", []):
        if (
            not isinstance(row, dict)
            or not isinstance(row.get("path"), str)
            or not isinstance(row.get("selector"), str)
        ):
            continue
        strength = order.get(row.get("classification"), 5)
        identity = f"{row['path']}::{row['selector']}"
        for name, limit in limits.items():
            if strength <= limit:
                result[name].append(identity)
    return {name: deduplicate(values) for name, values in result.items()}


def source_line_count(path: Path) -> int:
    data = path.read_bytes()
    return data.count(b"\n") + int(bool(data) and not data.endswith(b"\n"))


def line_budget_metrics(
    prefix: str,
    ranked: Sequence[str],
    relevant: Sequence[str],
    checkout_root: Path,
    budgets: Sequence[int],
) -> Mapping[str, Any]:
    unique_ranked = deduplicate(ranked)
    truth = set(relevant)
    result: dict[str, Any] = {}
    line_counts = {
        path: source_line_count(checkout_root / path)
        for path in unique_ranked
        if (checkout_root / path).is_file()
    }
    for budget in budgets:
        selected: list[str] = []
        used = 0
        for path in unique_ranked:
            lines = line_counts.get(path)
            if lines is None or used + lines > budget:
                continue
            selected.append(path)
            used += lines
        hits = truth.intersection(selected)
        result[f"{prefix}_files_at_{budget}_lines"] = len(selected)
        result[f"{prefix}_lines_used_at_{budget}"] = used
        result[f"{prefix}_recall_at_{budget}_lines"] = (
            None if not truth else len(hits) / len(truth)
        )
    return result


def checkout(repository: Path, destination: Path, revision: str) -> None:
    git(repository, "worktree", "prune", capture=False)
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    git(repository, "worktree", "add", "--force", "--detach", str(destination), revision, capture=False)


def remove_checkout(repository: Path, destination: Path) -> None:
    subprocess.run(
        ["git", f"--git-dir={repository}", "worktree", "remove", "--force", str(destination)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if destination.exists():
        shutil.rmtree(destination)
    git(repository, "worktree", "prune", capture=False)


def introduced_test_identities(value: object) -> tuple[list[str], str | None]:
    if not isinstance(value, list) or not value:
        return [], "no reviewed tests were introduced by this change"
    identities: list[str] = []
    for raw in value:
        if not isinstance(raw, str) or "::" not in raw:
            return [], "an introduced test lacks an exact path::selector identity"
        path, selector = raw.split("::", 1)
        try:
            normalized = safe_relative(path, "introduced test path")
        except EvaluationError:
            return [], "an introduced test has an invalid repository path"
        if not selector:
            return [], "an introduced test has an empty selector"
        identities.append(f"{normalized}::{selector}")
    if identities != sorted(set(identities), key=str.encode):
        return [], "introduced test identities are not sorted and unique"
    return identities, None


def verification_test_configuration(
    base: Mapping[str, Any], case_id: str, identities: Sequence[str]
) -> Mapping[str, Any]:
    paths = sorted(
        {identity.split("::", 1)[0] for identity in identities}, key=str.encode
    )
    configuration = json.loads(json.dumps(base))

    def keyed(collection: str) -> dict[str, Any]:
        current = configuration.get(collection, {})
        if isinstance(current, dict):
            return dict(current)
        if isinstance(current, list):
            result: dict[str, Any] = {}
            for row in current:
                if not isinstance(row, dict) or not isinstance(row.get("id"), str):
                    raise EvaluationError(
                        f"project configuration {collection} is not a named collection"
                    )
                value = dict(row)
                identity = str(value.pop("id"))
                if identity in result:
                    raise EvaluationError(
                        f"project configuration {collection} contains duplicate IDs"
                    )
                result[identity] = value
            return result
        raise EvaluationError(
            f"project configuration {collection} is not an object or array"
        )

    projections = keyed("projections")
    constraints = keyed("constraints")
    if "introduced-tests" in projections:
        raise EvaluationError(
            f"historical evaluation projection collides in case {case_id}"
        )
    if "HISTORICAL-INTRODUCED-TESTS" in constraints:
        raise EvaluationError(
            f"historical evaluation constraint collides in case {case_id}"
        )
    projections["introduced-tests"] = {
        "select": "test_selectors",
        "paths": paths,
    }
    constraints["HISTORICAL-INTRODUCED-TESTS"] = {
        "assert": "required_subset",
        "expected": {"literal": list(identities)},
        "actual": {"projection": "introduced-tests"},
        "owner": "historical-evaluation",
        "rationale": (
            "Reviewed after-only test identities must be absent before and "
            "present after the historical change; this does not claim that "
            "the tests execute or cover the behavior."
        ),
    }
    configuration["projections"] = projections
    configuration["constraints"] = constraints
    return configuration


def finding_by_key(
    document: Mapping[str, Any], key: str
) -> Mapping[str, Any] | None:
    for constraint in document.get("constraints", []):
        if not isinstance(constraint, dict):
            continue
        for finding in constraint.get("findings", []):
            if (
                isinstance(finding, dict)
                and finding.get("key") == key
                and finding.get("comparison") == "missing"
            ):
                return finding
    return None


def run_historical_verification(
    case_id: str,
    introduced: object,
    archbird: Path,
    case_output: Path,
    before_root: Path,
    before_map: Path,
    after_map: Path,
    project_configuration: Mapping[str, Any],
) -> tuple[Mapping[str, Any], Mapping[str, Any], Mapping[str, Any]]:
    identities, reason = introduced_test_identities(introduced)
    if reason is not None:
        return (
            {
                "act": {"applicability": "not_applicable", "reason": reason},
                "applicability": "not_applicable",
                "reason": reason,
            },
            {},
            {},
        )
    configuration = verification_test_configuration(
        project_configuration, case_id, identities
    )
    verification_root = case_output / "verification"
    documents: dict[str, Mapping[str, Any]] = {}
    observations: dict[str, Mapping[str, Any]] = {}
    artifact_rows: dict[str, Any] = {}
    for state, source_map in (("before", before_map), ("after", after_map)):
        directory = verification_root / state
        directory.mkdir(parents=True, exist_ok=True)
        map_path = directory / "map.json"
        config_path = directory / "archbird.json"
        result_path = directory / "result.json"
        shutil.copyfile(source_map, map_path)
        write_json(config_path, configuration)
        observation = run_process(
            [
                str(archbird),
                "verify",
                "--config",
                str(config_path),
                "--map",
                str(map_path),
                "--format",
                "json",
                "--output",
                str(result_path),
            ],
            directory,
            directory / "verify.stdout",
            directory / "verify.stderr",
        )
        observations[state] = observation
        if observation["returncode"] != 0 or not result_path.is_file():
            return (
                {
                    "act": {
                        "applicability": "unknown",
                        "reason": f"{state} verification failed",
                    },
                    "applicability": "applicable",
                    "error": f"{state} verification failed",
                },
                {},
                {"verification": observations},
            )
        document = read_json(result_path, f"{state} historical verification")
        documents[state] = document
        artifact_rows[state] = {
            "bytes": result_path.stat().st_size,
            "sha256": sha256_file(result_path),
        }
    before_findings = {
        finding["key"]
        for constraint in documents["before"].get("constraints", [])
        if isinstance(constraint, dict)
        for finding in constraint.get("findings", [])
        if isinstance(finding, dict)
        and isinstance(finding.get("key"), str)
        and finding.get("comparison") == "missing"
        and finding.get("evidence_state") == "current"
    }
    after_findings = {
        finding["key"]
        for constraint in documents["after"].get("constraints", [])
        if isinstance(constraint, dict)
        for finding in constraint.get("findings", [])
        if isinstance(finding, dict)
        and isinstance(finding.get("key"), str)
        and finding.get("comparison") == "missing"
        and finding.get("evidence_state") == "current"
    }
    truth = set(identities)
    before_constraint_states = {
        str(constraint.get("status"))
        for constraint in documents["before"].get("constraints", [])
        if isinstance(constraint, dict)
    }
    after_constraint_states = {
        str(constraint.get("status"))
        for constraint in documents["after"].get("constraints", [])
        if isinstance(constraint, dict)
    }
    evidence_current = before_constraint_states == {"fail"} and (
        after_constraint_states == {"pass"}
    )
    metrics: dict[str, Any] = {
        "verification_after_presence_recall": (
            len(truth - after_findings) / len(truth)
            if after_constraint_states == {"pass"}
            else None
        ),
        "verification_before_missing_recall": (
            len(truth.intersection(before_findings)) / len(truth)
            if before_constraint_states == {"fail"}
            else None
        ),
        "verification_false_findings": len(before_findings - truth)
        + len(after_findings - truth),
        "verification_transition_accuracy": (
            float(
                before_findings == truth and not after_findings
            )
            if evidence_current
            else None
        ),
    }

    act_root = case_output / "act"
    act_root.mkdir(parents=True, exist_ok=True)
    expected_paths = [identity.split("::", 1)[0] for identity in identities]
    expected_fingerprints = {
        str(finding["fingerprint"])
        for identity in identities
        for finding in [finding_by_key(documents["before"], identity)]
        if finding is not None and isinstance(finding.get("fingerprint"), str)
    }
    plan_path = act_root / "plan.json"
    plan_observation = run_process(
        [
            str(archbird),
            "plan",
            "HISTORICAL-INTRODUCED-TESTS",
            "--config",
            str(verification_root / "before/archbird.json"),
            "--map",
            str(verification_root / "before/map.json"),
            "--root",
            str(before_root),
            "--output",
            str(plan_path),
        ],
        act_root,
        act_root / "plan.stdout",
        act_root / "plan.stderr",
    )
    plan: Mapping[str, Any] | None = None
    plan_artifact: Mapping[str, Any] | None = None
    if plan_observation["returncode"] == 0 and plan_path.is_file():
        plan = read_json(plan_path, "historical Plan")
        plan_artifact = {
            "bytes": plan_path.stat().st_size,
            "sha256": sha256_file(plan_path),
        }
    plan_items = (
        [
            item
            for item in plan.get("items", [])
            if isinstance(item, dict)
        ]
        if plan is not None
        else []
    )
    plan_fingerprints = {
        str(origin["issue_fingerprint"])
        for item in plan_items
        for origin in item.get("origins", [])
        if isinstance(origin, dict)
        and isinstance(origin.get("issue_fingerprint"), str)
    }
    plan_paths: list[str] = []
    acceptance_ids: set[str] = set()
    for item in plan_items:
        operation = item.get("operation")
        if isinstance(operation, dict):
            for field in ("path", "source_path", "destination_path"):
                if isinstance(operation.get(field), str):
                    plan_paths.append(str(operation[field]))
            plan_paths.extend(
                str(value)
                for value in operation.get("candidate_paths", [])
                if isinstance(value, str)
            )
        acceptance = item.get("acceptance")
        if isinstance(acceptance, dict):
            acceptance_ids.update(
                str(value)
                for value in acceptance.get("constraints", [])
                if isinstance(value, str)
            )
    executable_items = [
        item for item in plan_items if item.get("executable") is True
    ]
    manual_items = [
        item
        for item in plan_items
        if isinstance(item.get("operation"), dict)
        and item["operation"].get("action") == "manual"
    ]
    metrics["plan_generation_success"] = float(plan is not None)
    metrics["plan_items"] = len(plan_items)
    metrics["plan_executable_items"] = len(executable_items)
    metrics["plan_non_executable_items"] = (
        len(plan_items) - len(executable_items)
    )
    metrics["plan_manual_items"] = len(manual_items)
    metrics["plan_executable_fraction"] = (
        len(executable_items) / len(plan_items) if plan_items else None
    )
    metrics["plan_origin_recall"] = (
        len(expected_fingerprints.intersection(plan_fingerprints))
        / len(expected_fingerprints)
        if expected_fingerprints
        else None
    )
    metrics["plan_acceptance_coverage"] = float(
        "HISTORICAL-INTRODUCED-TESTS" in acceptance_ids
    )
    metrics["plan_after_acceptance_satisfied"] = float(
        plan is not None
        and "HISTORICAL-INTRODUCED-TESTS" in acceptance_ids
        and after_constraint_states == {"pass"}
    )
    metrics.update(rank_metrics("plan_candidate_path", plan_paths, expected_paths))

    preview_path = act_root / "preview.json"
    preview_observation: Mapping[str, Any] | None = None
    preview: Mapping[str, Any] | None = None
    if plan is not None:
        preview_observation = run_process(
            [
                str(archbird),
                "act",
                str(plan_path),
                "--root",
                str(before_root),
                "--config",
                str(verification_root / "before/archbird.json"),
                "--format",
                "json",
                "--output",
                str(preview_path),
            ],
            act_root,
            act_root / "preview.stdout",
            act_root / "preview.stderr",
        )
        if preview_path.is_file():
            preview = read_json(preview_path, "historical Act preview")
    metrics["act_preview_available"] = float(preview is not None)
    act: dict[str, Any] = {
        "applicability": "applicable",
        "preview": (
            {
                "applicability": "applicable",
                "artifact": {
                    "bytes": preview_path.stat().st_size,
                    "sha256": sha256_file(preview_path),
                },
                "observation": preview_observation,
                "status": preview.get("state"),
            }
            if preview is not None
            else {
                "applicability": "unknown",
                "observation": preview_observation,
                "reason": "Act preview was not produced",
            }
        ),
    }
    track = {
        "act": act,
        "applicability": "applicable",
        "expected_test_identities": identities,
        "plan": {
            "applicability": (
                "applicable" if plan is not None else "unknown"
            ),
            "artifact": plan_artifact,
            "executable_items": len(executable_items),
            "items": len(plan_items),
            "manual_items": len(manual_items),
            "non_executable_items": len(plan_items) - len(executable_items),
            "observation": plan_observation,
        },
        "verification": {
            "after_missing": sorted(
                truth.intersection(after_findings), key=str.encode
            ),
            "after_status": sorted(after_constraint_states, key=str.encode),
            "before_missing": sorted(
                truth.intersection(before_findings), key=str.encode
            ),
            "before_status": sorted(before_constraint_states, key=str.encode),
        },
    }
    artifacts = {
        "act_preview": (
            {
                "bytes": preview_path.stat().st_size,
                "sha256": sha256_file(preview_path),
            }
            if preview is not None
            else None
        ),
        "plan": plan_artifact,
        "verification": artifact_rows,
    }
    return track, metrics, {"artifacts": artifacts, "observations": observations}


def run_case(
    root: Path,
    corpus_directory: Path,
    entry: Mapping[str, Any],
    archbird: Path,
    stage: Path,
    cache_root: Path,
) -> Mapping[str, Any]:
    case_id = str(entry["id"])
    case_path = corpus_directory / "cases" / f"{case_id}.json"
    if sha256_file(case_path) != entry["case_sha256"]:
        raise EvaluationError(f"frozen case is corrupt: {case_id}")
    case = validate_case(read_json(case_path, "frozen evaluation case"), case_path)
    protocol_path = corpus_directory / "protocol.json"
    protocol = validate_protocol(
        read_json(protocol_path, "frozen evaluation protocol"), protocol_path
    )
    repository_info = case["repository"]
    repository = repository_path(root, str(repository_info["id"]))
    worktree = root / "work/checkouts" / case_id
    before_action_worktree = root / "work/checkouts" / f"{case_id}-plan-before"
    case_output = stage / "cases" / case_id
    case_output.mkdir(parents=True, exist_ok=True)
    try:
        checkout(repository, worktree, str(repository_info["before_revision"]))
        map_path = case_output / "map.json"
        map_command = [
            str(archbird),
            "map",
            str(worktree),
            "--cache-dir",
            str(cache_root),
            "--cache-max-bytes",
            str(PROVIDER_CACHE_MAX_BYTES),
        ]
        config = case["analysis"].get("config")
        config_path = case_output / "archbird.json"
        if config is None:
            config_observation = run_process(
                [
                    str(archbird),
                    "config",
                    "init",
                    str(worktree),
                    "--no-config",
                    "--output",
                    str(config_path),
                ],
                worktree,
                case_output / "config.stdout",
                case_output / "config.stderr",
            )
            if config_observation["returncode"] != 0 or not config_path.is_file():
                return {
                    "case_sha256": entry["case_sha256"],
                    "config": config_observation,
                    "error": "project configuration initialization failed",
                    "id": case_id,
                    "review_status": entry["review_status"],
                    "split": entry["split"],
                    "status": "error",
                }
            config = read_json(config_path, "generated project configuration")
        else:
            write_json(config_path, config)
        map_command.extend(("--config", str(config_path)))
        map_command.extend(("--format", "json", "--output", str(map_path)))
        map_observation = run_process(
            map_command,
            worktree,
            case_output / "map.stdout",
            case_output / "map.stderr",
        )
        if map_observation["returncode"] != 0 or not map_path.is_file():
            return {
                "case_sha256": entry["case_sha256"],
                "error": "map command failed",
                "id": case_id,
                "map": map_observation,
                "review_status": entry["review_status"],
                "split": entry["split"],
                "status": "error",
            }
        map_document = read_json(map_path, "case Map")
        analysis = case["analysis"]
        path_track = analysis["track"] == "exact_path"
        if path_track:
            analysis_kind = "path"
            analysis_path = case_output / "path.json"
            analysis_command = [
                str(archbird),
                "path",
                str(analysis["source"]["value"]),
                str(analysis["target"]["value"]),
                "--map",
                str(map_path),
                "--source-kind",
                str(analysis["source"]["kind"]),
                "--target-kind",
                str(analysis["target"]["kind"]),
                "--level",
                str(analysis["level"]),
                "--direction",
                str(analysis["direction"]),
                "--max-depth",
                str(analysis["max_depth"]),
                "--max-paths",
                str(analysis["max_paths"]),
            ]
            for relation in analysis["relations"]:
                analysis_command.extend(("--relation", str(relation)))
        else:
            analysis_kind = "query"
            analysis_path = case_output / "query.json"
            command_name = (
                "query" if analysis["track"] == "exact_query" else "impact"
            )
            analysis_command = [
                str(archbird),
                command_name,
                "--map",
                str(map_path),
            ]
            for selector in analysis["selectors"]:
                analysis_command.extend(
                    (f"--{selector['kind']}", selector["value"])
                )
            analysis_command.extend(
                (
                    "--depth",
                    str(analysis["depth"]),
                    "--test-depth",
                    str(analysis["test_depth"]),
                )
            )
        analysis_command.extend(
            ("--format", "json", "--output", str(analysis_path))
        )
        analysis_observation = run_process(
            analysis_command,
            worktree,
            case_output / f"{analysis_kind}.stdout",
            case_output / f"{analysis_kind}.stderr",
        )
        if (
            analysis_observation["returncode"] != 0
            or not analysis_path.is_file()
        ):
            return {
                "case_sha256": entry["case_sha256"],
                "error": f"{analysis_kind} command failed",
                "id": case_id,
                "map": map_observation,
                analysis_kind: analysis_observation,
                "review_status": entry["review_status"],
                "split": entry["split"],
                "status": "error",
            }
        analysis_document = read_json(
            analysis_path, f"case {analysis_kind.title()}"
        )
        if path_track:
            ranked_files: list[str] = []
            ranked_context_files: list[str] = []
            seed_files: list[str] = []
            ranked_symbols: list[str] = []
            matched_symbols: list[str] = []
            ranked_tests: list[str] = []
            canonical_evidence_counts: Mapping[str, int] = {}
            seed_truth: list[str] = []
            matched_symbol_truth: list[str] = []
        else:
            (
                ranked_files,
                ranked_context_files,
                seed_files,
                ranked_symbols,
                matched_symbols,
                ranked_tests,
                canonical_evidence_counts,
            ) = extract_rankings(analysis_document)
            seed_truth, matched_symbol_truth = selector_truth(
                analysis["selectors"], worktree
            )
        issue_query_path = case_output / "issue-query.json"
        issue_query_command = [
            str(archbird),
            "query",
            "--map",
            str(map_path),
            "--search",
            str(case["task"]["title"]),
            "--search-limit",
            str(protocol["analysis"]["issue_search_limit"]),
            "--depth",
            str(protocol["analysis"]["depth"]),
            "--test-depth",
            str(protocol["analysis"]["test_depth"]),
            "--format",
            "json",
            "--output",
            str(issue_query_path),
        ]
        issue_query_observation = run_process(
            issue_query_command,
            worktree,
            case_output / "issue-query.stdout",
            case_output / "issue-query.stderr",
        )
        if (
            issue_query_observation["returncode"] != 0
            or not issue_query_path.is_file()
        ):
            return {
                "case_sha256": entry["case_sha256"],
                "error": "issue query command failed",
                "id": case_id,
                "issue_query": issue_query_observation,
                "map": map_observation,
                analysis_kind: analysis_observation,
                "review_status": entry["review_status"],
                "split": entry["split"],
                "status": "error",
            }
        issue_query_document = read_json(issue_query_path, "case issue Query")
        (
            issue_files,
            issue_context_files,
            _issue_seed_files,
            issue_symbols,
            _issue_matched_symbols,
            issue_tests,
            issue_canonical_evidence_counts,
        ) = extract_rankings(issue_query_document)
        issue_retrieval_files, issue_retrieval_symbols = (
            extract_retrieval_rankings(issue_query_document)
        )
        test_tiers = (
            {} if path_track else extract_test_tiers(analysis_document)
        )
        truth = case["ground_truth"]
        line_budgets = protocol["metrics"].get(
            "line_budgets", [200, 500, 1000, 2000]
        )
        metrics: dict[str, Any] = {}
        if path_track:
            metrics.update(
                exact_path_metrics(analysis_document, truth["path"])
            )
        else:
            metrics.update(
                rank_metrics(
                    "relevant_file", ranked_files, truth["relevant_files"]
                )
            )
            metrics.update(
                rank_metrics(
                    "relevant_context_file",
                    ranked_context_files,
                    truth["relevant_files"],
                )
            )
            metrics.update(rank_metrics("seed_file", seed_files, seed_truth))
            metrics.update(
                rank_metrics("diff_file", ranked_files, entry["diff_files"])
            )
            metrics.update(
                rank_metrics(
                    "diff_context_file",
                    ranked_context_files,
                    entry["diff_files"],
                )
            )
            metrics.update(
                rank_metrics(
                    "relevant_symbol",
                    ranked_symbols,
                    truth["relevant_symbols"],
                )
            )
            metrics.update(
                rank_metrics(
                    "matched_symbol", matched_symbols, matched_symbol_truth
                )
            )
            metrics.update(
                rank_metrics(
                    "relevant_test", ranked_tests, truth["relevant_tests"]
                )
            )
            for tier, values in test_tiers.items():
                metrics.update(
                    rank_metrics(
                        f"relevant_test_{tier}",
                        values,
                        truth["relevant_tests"],
                    )
                )
            metrics.update(canonical_evidence_counts)
            metrics.update(
                line_budget_metrics(
                    "relevant_file_context",
                    ranked_context_files,
                    truth["relevant_files"],
                    worktree,
                    line_budgets,
                )
            )
            metrics.update(
                line_budget_metrics(
                    "diff_file_context",
                    ranked_context_files,
                    entry["diff_files"],
                    worktree,
                    line_budgets,
                )
            )
        metrics.update(
            rank_metrics(
                "issue_retrieval_file",
                issue_retrieval_files,
                truth["relevant_files"],
            )
        )
        metrics.update(
            rank_metrics(
                "issue_retrieval_symbol",
                issue_retrieval_symbols,
                truth["relevant_symbols"],
            )
        )
        metrics.update(rank_metrics("issue_file", issue_files, truth["relevant_files"]))
        metrics.update(
            rank_metrics(
                "issue_context_file",
                issue_context_files,
                truth["relevant_files"],
            )
        )
        metrics.update(
            rank_metrics("issue_symbol", issue_symbols, truth["relevant_symbols"])
        )
        metrics.update(rank_metrics("issue_test", issue_tests, truth["relevant_tests"]))
        metrics.update(
            {
                f"issue_{name}": value
                for name, value in issue_canonical_evidence_counts.items()
            }
        )
        metrics.update(
            {
                "issue_query_duration_ms": issue_query_observation["duration_ms"],
                "issue_query_max_rss_kib": issue_query_observation["max_rss_kib"],
                "map_duration_ms": map_observation["duration_ms"],
                "map_max_rss_kib": map_observation["max_rss_kib"],
                f"{analysis_kind}_duration_ms": analysis_observation[
                    "duration_ms"
                ],
                f"{analysis_kind}_max_rss_kib": analysis_observation[
                    "max_rss_kib"
                ],
            }
        )
        remove_checkout(repository, worktree)
        checkout(repository, worktree, str(repository_info["after_revision"]))
        after_map_path = case_output / "after-map.json"
        after_map_command = [
            str(archbird),
            "map",
            str(worktree),
            "--cache-dir",
            str(cache_root),
            "--cache-max-bytes",
            str(PROVIDER_CACHE_MAX_BYTES),
        ]
        after_map_command.extend(("--config", str(config_path)))
        after_map_command.extend(
            ("--format", "json", "--output", str(after_map_path))
        )
        after_map_observation = run_process(
            after_map_command,
            worktree,
            case_output / "after-map.stdout",
            case_output / "after-map.stderr",
        )
        if (
            after_map_observation["returncode"] != 0
            or not after_map_path.is_file()
        ):
            return {
                "case_sha256": entry["case_sha256"],
                "error": "after Map command failed",
                "id": case_id,
                "map": map_observation,
                "map_after": after_map_observation,
                analysis_kind: analysis_observation,
                "review_status": entry["review_status"],
                "split": entry["split"],
                "status": "error",
            }
        checkout(
            repository,
            before_action_worktree,
            str(repository_info["before_revision"]),
        )
        verification_track, verification_metrics, verification_runtime = (
            run_historical_verification(
                case_id,
                truth.get("introduced_tests", []),
                archbird,
                case_output,
                before_action_worktree,
                map_path,
                after_map_path,
                config,
            )
        )
        metrics.update(verification_metrics)
        extra_artifacts = verification_runtime.get("artifacts", {})
        return {
            "artifacts": {
                "after_map": {
                    "bytes": after_map_path.stat().st_size,
                    "sha256": sha256_file(after_map_path),
                },
                "issue_query": {
                    "bytes": issue_query_path.stat().st_size,
                    "sha256": sha256_file(issue_query_path),
                },
                "map": {"bytes": map_path.stat().st_size, "sha256": sha256_file(map_path)},
                analysis_kind: {
                    "bytes": analysis_path.stat().st_size,
                    "sha256": sha256_file(analysis_path),
                },
                **extra_artifacts,
            },
            "case_sha256": entry["case_sha256"],
            "id": case_id,
            "issue_query": issue_query_observation,
            "map": map_observation,
            "map_after": after_map_observation,
            "map_tool": map_document.get("source_tool", map_document.get("tool")),
            "metrics": metrics,
            analysis_kind: analysis_observation,
            "review_status": entry["review_status"],
            "split": entry["split"],
            "status": "ok",
            "transition_track": verification_track,
            "transition_observations": verification_runtime.get(
                "observations", {}
            ),
        }
    finally:
        remove_checkout(repository, before_action_worktree)
        remove_checkout(repository, worktree)


def aggregate_cases(
    cases: Sequence[Mapping[str, Any]],
    status: str | None,
    split: str | None = None,
) -> Mapping[str, Any]:
    selected = [
        case
        for case in cases
        if case.get("status") == "ok"
        and (status is None or case.get("review_status") == status)
        and (split is None or case.get("split") == split)
    ]
    metric_names = sorted(
        {
            key
            for case in selected
            for key, value in case.get("metrics", {}).items()
            if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)
        },
        key=str.encode,
    )
    metrics = {}
    for name in metric_names:
        values = [float(case["metrics"][name]) for case in selected if isinstance(case["metrics"].get(name), (int, float))]
        if values:
            metrics[name] = statistics.fmean(values)
    pooled = {}
    for prefix in (
        "diff_context_file",
        "diff_file",
        "issue_context_file",
        "issue_file",
        "issue_retrieval_file",
        "issue_retrieval_symbol",
        "issue_symbol",
        "issue_test",
        "matched_symbol",
        "path_node",
        "plan_candidate_path",
        "relevant_context_file",
        "relevant_file",
        "relevant_symbol",
        "relevant_test",
        "relevant_test_all",
        "relevant_test_candidate",
        "relevant_test_direct",
        "relevant_test_observed_asserted",
        "seed_file",
    ):
        truth = sum(
            int(case["metrics"].get(f"{prefix}_truth", 0))
            for case in selected
        )
        returned = sum(
            int(case["metrics"].get(f"{prefix}_returned", 0))
            for case in selected
        )
        if not truth:
            continue
        row = {"returned": returned, "truth": truth}
        for suffix in ("recall_at_5", "recall_at_10", "recall_at_20", "recall_all"):
            hits = sum(
                round(
                    float(case["metrics"][f"{prefix}_{suffix}"])
                    * int(case["metrics"][f"{prefix}_truth"])
                )
                for case in selected
                if isinstance(case["metrics"].get(f"{prefix}_{suffix}"), (int, float))
            )
            row[f"hits_{suffix.removeprefix('recall_')}"] = hits
            row[suffix] = hits / truth
        pooled[prefix] = row
    return {
        "cases": len(selected),
        "metrics_mean": metrics,
        "metrics_pooled": pooled,
    }


def run_evaluation(
    root: Path,
    archbird: Path,
    label: str,
    corpus_sha256: str | None,
    open_held_out: bool,
) -> str:
    reject_after_held_out_open(root, "another evaluation run")
    corpus_sha256, corpus, corpus_directory = load_corpus(root, corpus_sha256)
    entries = list(corpus["cases"])
    held_out = [entry for entry in entries if entry.get("split") == "held_out"]
    if open_held_out:
        current = read_state(root).get("current_corpus_sha256")
        if current != corpus_sha256:
            raise EvaluationError(
                "held-out scoring requires the current frozen corpus"
            )
        if not held_out:
            raise EvaluationError("current corpus contains no held-out cases")
        selected_entries = entries
    else:
        selected_entries = [
            entry for entry in entries if entry.get("split") != "held_out"
        ]
        if not selected_entries:
            raise EvaluationError(
                "corpus contains only held-out cases; use --open-held-out once"
            )
    if not archbird.is_file() or not os.access(archbird, os.X_OK):
        raise EvaluationError(f"Archbird executable is unavailable: {archbird}")
    environment = evaluated_environment()
    version = subprocess.run(
        [str(archbird), "--version"],
        check=True,
        capture_output=True,
        env=environment,
        text=True,
    ).stdout.strip()
    support_process = subprocess.run(
        [str(archbird), "support"],
        capture_output=True,
        env=environment,
        text=True,
    )
    support: Mapping[str, Any]
    if support_process.returncode == 0:
        try:
            support_value = json.loads(support_process.stdout)
        except json.JSONDecodeError as error:
            raise EvaluationError(
                f"Archbird support output is invalid JSON: {error}"
            ) from error
        if not isinstance(support_value, dict):
            raise EvaluationError("Archbird support output must be an object")
        support = {"report": support_value, "status": "available"}
    else:
        support = {
            "returncode": support_process.returncode,
            "status": "unavailable",
            "stderr_sha256": sha256_bytes(support_process.stderr.encode("utf-8")),
            "stdout_sha256": sha256_bytes(support_process.stdout.encode("utf-8")),
        }
    launcher = {
        "path": str(archbird),
        "sha256": sha256_file(archbird),
    }
    stage = Path(tempfile.mkdtemp(prefix=".run-", dir=root / "work"))
    cache_stage = Path(tempfile.mkdtemp(prefix=".cache-", dir=root / "work"))
    try:
        if open_held_out:
            claim_held_out_opening(root, corpus_sha256, label)
        cases = []
        for entry in selected_entries:
            print(f"evaluating {entry['id']} ...", flush=True)
            cases.append(
                run_case(
                    root,
                    corpus_directory,
                    entry,
                    archbird,
                    stage,
                    cache_stage / str(entry["id"]),
                )
            )
        tool_evidence = next((case.get("map_tool") for case in cases if case.get("map_tool") is not None), None)
        result = {
            "aggregate": {
                "all": aggregate_cases(cases, None),
                "by_split": {
                    split: aggregate_cases(cases, None, split)
                    for split in ("development", "validation", "held_out")
                },
                "candidate": aggregate_cases(cases, "candidate"),
                "reviewed": aggregate_cases(cases, "reviewed"),
                "reviewed_by_split": {
                    split: aggregate_cases(cases, "reviewed", split)
                    for split in ("development", "validation", "held_out")
                },
            },
            "artifact": RUN_ARTIFACT,
            "cache_policy": CACHE_POLICY,
            "cases": cases,
            "corpus_sha256": corpus_sha256,
            "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
            "host": evaluation_host(),
            "held_out_opened": open_held_out,
            "label": label,
            "provenance": "observed",
            "schema_version": RUN_SCHEMA_VERSION,
            "tool": {
                "cli_version": version,
                "launcher": launcher,
                "map_tool": tool_evidence,
                "support": support,
            },
        }
        encoded = canonical(result) + b"\n"
        run_sha256 = sha256_bytes(encoded)
        write_json(stage / "result.json", result)
        destination = root / "runs" / run_sha256
        if destination.exists():
            if sha256_file(destination / "result.json") != run_sha256:
                raise EvaluationError(f"existing evaluation run is corrupt: {run_sha256}")
            shutil.rmtree(stage)
        else:
            os.replace(stage, destination)
        state = dict(read_state(root))
        previous = state.get("current_run_sha256")
        if isinstance(previous, str):
            comparison = compare_runs(root, previous, run_sha256)
            state["current_run_comparison"] = publish_comparison(
                root, "runs", previous, run_sha256, comparison
            )
        state.update(
            {
                "current_run_sha256": run_sha256,
                "generation": int(state["generation"]) + 1,
                "held_out_run_sha256": run_sha256 if open_held_out else None,
                "previous_run_sha256": previous,
            }
        )
        write_json(root / "state.json", state)
        print(f"evaluation run: {run_sha256} ({len(cases)} cases)")
        if isinstance(previous, str):
            print(f"comparison: {previous}--{run_sha256}")
        if any(case.get("status") != "ok" for case in cases):
            raise EvaluationError("evaluation run contains failed cases")
        return run_sha256
    finally:
        if stage.exists():
            shutil.rmtree(stage)
        shutil.rmtree(cache_stage, ignore_errors=True)


def canonical_test_tier_metric(name: str) -> bool:
    return any(
        name.startswith(f"relevant_test_{tier}_")
        for tier in ("all", "candidate", "direct", "observed_asserted")
    )


def metric_direction(name: str) -> str | None:
    if name in {
        "act_preview_available",
        "plan_acceptance_coverage",
        "plan_executable_fraction",
        "plan_generation_success",
    }:
        return "higher"
    if canonical_test_tier_metric(name):
        return None
    if any(
        token in name
        for token in ("recall", "mrr", "precision", "accuracy", "satisfied")
    ):
        return "higher"
    if any(
        token in name
        for token in ("distractor", "duration", "false_findings", "rss")
    ):
        return "lower"
    return None


def metric_family(name: str) -> str:
    if name in {
        "act_preview_available",
        "plan_acceptance_coverage",
        "plan_executable_fraction",
        "plan_generation_success",
    }:
        return "quality"
    if any(token in name for token in ("duration", "rss")):
        return "performance"
    if canonical_test_tier_metric(name):
        return "scale"
    if "distractor" in name:
        return "context"
    if "false_findings" in name:
        return "quality"
    if any(
        token in name
        for token in ("recall", "mrr", "precision", "accuracy", "satisfied")
    ):
        return "quality"
    return "scale"


def comparison_status(classes: set[str]) -> str:
    if "regressed" in classes:
        return "regressed"
    if "improved" in classes:
        return "improved"
    if "changed" in classes:
        return "changed"
    return "unchanged"


def compare_metric(name: str, before: float, after: float) -> Mapping[str, Any]:
    delta = after - before
    direction = metric_direction(name)
    tolerance = max(abs(before) * 0.05, 0.001) if any(token in name for token in ("duration", "rss")) else 1e-12
    family = metric_family(name)
    if family == "performance":
        classification = "unchanged" if abs(delta) <= tolerance else "changed"
    elif direction is None or abs(delta) <= tolerance:
        classification = "unchanged" if abs(delta) <= tolerance else "changed"
    elif (direction == "higher" and delta > 0) or (direction == "lower" and delta < 0):
        classification = "improved"
    else:
        classification = "regressed"
    return {
        "after": after,
        "before": before,
        "classification": classification,
        "delta": delta,
        "direction": direction,
        "family": family,
        "tolerance": tolerance,
    }


def evaluation_host() -> Mapping[str, Any]:
    try:
        available_cpus: Optional[int] = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        available_cpus = None
    return {
        "available_cpus": available_cpus,
        "logical_cpus": os.cpu_count(),
        "machine": platform.machine(),
        "node_sha256": sha256_bytes(platform.node().encode("utf-8")),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "runtime": {
            "implementation": platform.python_implementation(),
            "version": platform.python_version(),
        },
    }


def comparable_performance_environment(
    run: Mapping[str, Any],
) -> Optional[Mapping[str, Any]]:
    evaluator = run.get("evaluator")
    host = run.get("host")
    tool_record = run.get("tool")
    if (
        not isinstance(evaluator, dict)
        or set(evaluator) != {"implementation_sha256"}
        or not isinstance(evaluator["implementation_sha256"], str)
        or not SHA256_RE.fullmatch(evaluator["implementation_sha256"])
        or not isinstance(host, dict)
        or not isinstance(tool_record, dict)
    ):
        return None
    required_host = {
        "available_cpus",
        "logical_cpus",
        "machine",
        "node_sha256",
        "platform",
        "processor",
        "runtime",
    }
    support = tool_record.get("support")
    if set(host) != required_host or not isinstance(support, dict):
        return None
    report = support.get("report")
    if support.get("status") != "available" or not isinstance(report, dict):
        return None
    runtime = report.get("runtime")
    engine = report.get("engine")
    pattern = report.get("pattern")
    providers = report.get("providers")
    if (
        not isinstance(runtime, dict)
        or not isinstance(runtime.get("implementation"), str)
        or not isinstance(runtime.get("kind"), str)
        or not isinstance(runtime.get("version"), str)
        or not isinstance(engine, dict)
        or not isinstance(pattern, dict)
        or not isinstance(providers, dict)
    ):
        return None
    return {
        "evaluator": evaluator,
        "host": host,
        "tool": {
            "engine": engine,
            "pattern": pattern,
            "providers": providers,
            "runtime": {
                "implementation": runtime["implementation"],
                "kind": runtime["kind"],
                "version": runtime["version"],
            },
        },
    }


def compare_runs(root: Path, before_sha: str, after_sha: str) -> Mapping[str, Any]:
    for value in (before_sha, after_sha):
        if not SHA256_RE.fullmatch(value):
            raise EvaluationError("run digest is invalid")
    before_path = root / "runs" / before_sha / "result.json"
    after_path = root / "runs" / after_sha / "result.json"
    if sha256_file(before_path) != before_sha or sha256_file(after_path) != after_sha:
        raise EvaluationError("evaluation run is missing or corrupt")
    before = read_json(before_path, "before evaluation run")
    after = read_json(after_path, "after evaluation run")
    cache_comparable = (
        before.get("cache_policy") is not None
        and before.get("cache_policy") == after.get("cache_policy")
    )
    before_environment = comparable_performance_environment(before)
    after_environment = comparable_performance_environment(after)
    environment_comparable = (
        before_environment is not None
        and before_environment == after_environment
    )
    if before_environment is None or after_environment is None:
        environment_relation = "unknown"
    else:
        environment_relation = "same" if environment_comparable else "changed"
    performance_comparable = cache_comparable and environment_comparable
    old = {str(case["id"]): case for case in before["cases"]}
    new = {str(case["id"]): case for case in after["cases"]}
    rows = []
    def counters() -> dict[str, int]:
        return {"changed": 0, "improved": 0, "regressed": 0, "unchanged": 0}
    summary: dict[str, Any] = {
        "candidate_context": counters(),
        "candidate_quality": counters(),
        "context_by_split": {
            split: counters()
            for split in ("development", "validation", "held_out")
        },
        "incomparable": 0,
        "performance": {"changed": 0, "incomparable": 0, "unchanged": 0},
        "quality_by_split": {
            split: counters()
            for split in ("development", "validation", "held_out")
        },
        "reviewed_context": counters(),
        "reviewed_quality": counters(),
    }
    for case_id in sorted(set(old) | set(new), key=str.encode):
        left = old.get(case_id)
        right = new.get(case_id)
        if left is None or right is None or left.get("case_sha256") != right.get("case_sha256"):
            rows.append({"id": case_id, "status": "incomparable"})
            summary["incomparable"] += 1
            continue
        metrics = {}
        for name in sorted(set(left.get("metrics", {})) & set(right.get("metrics", {})), key=str.encode):
            before_value = left["metrics"][name]
            after_value = right["metrics"][name]
            if (
                isinstance(before_value, (int, float))
                and not isinstance(before_value, bool)
                and isinstance(after_value, (int, float))
                and not isinstance(after_value, bool)
            ):
                metric = compare_metric(
                    name, float(before_value), float(after_value)
                )
                if (
                    metric["family"] == "performance"
                    and not performance_comparable
                ):
                    metric = {**metric, "classification": "incomparable"}
                metrics[name] = metric
        quality_classes = {
            metric["classification"]
            for metric in metrics.values()
            if metric["family"] == "quality"
        }
        performance_classes = {
            metric["classification"]
            for metric in metrics.values()
            if metric["family"] == "performance"
        }
        context_classes = {
            metric["classification"]
            for metric in metrics.values()
            if metric["family"] == "context"
        }
        status = comparison_status(quality_classes)
        context_status = comparison_status(context_classes)
        if "incomparable" in performance_classes:
            performance_status = "incomparable"
        else:
            performance_status = (
                "changed" if "changed" in performance_classes else "unchanged"
            )
        summary["performance"][performance_status] += 1
        review_status = str(right.get("review_status", "candidate"))
        split = str(right.get("split", "development"))
        summary[f"{review_status}_quality"][status] += 1
        summary[f"{review_status}_context"][context_status] += 1
        summary["quality_by_split"][split][status] += 1
        summary["context_by_split"][split][context_status] += 1
        rows.append(
            {
                "context_status": context_status,
                "id": case_id,
                "metrics": metrics,
                "performance_status": performance_status,
                "review_status": review_status,
                "split": split,
                "status": status,
            }
        )
    return {
        "after_run_sha256": after_sha,
        "artifact": COMPARISON_ARTIFACT,
        "before_run_sha256": before_sha,
        "cache_relation": "same" if cache_comparable else "changed",
        "cases": rows,
        "corpus_relation": "same" if before["corpus_sha256"] == after["corpus_sha256"] else "changed",
        "evaluator": {"implementation_sha256": sha256_file(Path(__file__))},
        "performance_environment_relation": environment_relation,
        "provenance": "derived",
        "schema_version": COMPARISON_SCHEMA_VERSION,
        "summary": summary,
    }


def show(root: Path) -> None:
    state = read_state(root)
    print(json.dumps(state, indent=2, sort_keys=True))
    claim = read_held_out_claim(root)
    if claim is not None:
        print(json.dumps(claim, indent=2, sort_keys=True))
    current = state.get("current_run_sha256")
    previous = state.get("previous_run_sha256")
    if isinstance(current, str):
        result = read_json(root / "runs" / current / "result.json", "current run")
        print(json.dumps(result["aggregate"], indent=2, sort_keys=True))
    if isinstance(current, str) and isinstance(previous, str):
        comparison_path = state.get("current_run_comparison")
        if isinstance(comparison_path, str):
            comparison = read_json(root / safe_relative(comparison_path, "comparison path"), "current comparison")
            print(json.dumps(comparison["summary"], indent=2, sort_keys=True))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("init", help=f"initialize the root named by {ROOT_ENV}")
    commands.add_parser("validate", help="validate authoring cases and synchronized revisions")
    sync = commands.add_parser("sync", help="create or validate external bare repositories")
    sync.add_argument("--fetch", action="store_true", help="fetch missing pinned revisions")
    sync.add_argument(
        "--reservations",
        choices=("none", "validation", "all"),
        default="none",
        help="also synchronize no reserved repositories, validation only, or all",
    )
    commands.add_parser(
        "reserve",
        help="resolve remote HEADs and publish a content-addressed repository split",
    )
    commands.add_parser("freeze", help="publish a content-addressed corpus version")
    run = commands.add_parser("run", help="evaluate one Archbird executable and compare the prior run")
    run.add_argument("--archbird", type=Path, required=True)
    run.add_argument("--label", required=True)
    run.add_argument("--corpus")
    run.add_argument(
        "--open-held-out",
        action="store_true",
        help=(
            "irreversibly open and score held-out cases exactly once; ordinary "
            "runs keep them sealed"
        ),
    )
    compare = commands.add_parser("compare", help="compare two immutable run digests")
    compare.add_argument("--before", required=True)
    compare.add_argument("--after", required=True)
    compare.add_argument("--output", type=Path)
    compare.add_argument("--record", action="store_true")
    commands.add_parser("show", help="show current corpus, run, and comparison summaries")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        root = root_from_environment()
        if args.command == "init":
            initialize(root)
            print(f"evaluation root initialized: {root}")
            return 0
        if not (root / "state.json").is_file():
            raise EvaluationError(f"evaluation root is not initialized: {root}")
        initialize(root)
        if args.command == "validate":
            cases = authoring_cases(root)
            corpus_document(root)
            reservation_path = root / "authoring/reservations.json"
            if reservation_path.is_file():
                reservation_plan(root)
            print(f"evaluation authoring valid: {len(cases)} cases")
        elif args.command == "sync":
            sync_repositories(root, args.fetch, args.reservations)
        elif args.command == "reserve":
            freeze_reservation(root)
        elif args.command == "freeze":
            freeze_corpus(root)
        elif args.command == "run":
            run_evaluation(
                root,
                args.archbird.resolve(),
                args.label,
                args.corpus,
                args.open_held_out,
            )
        elif args.command == "compare":
            comparison = compare_runs(root, args.before, args.after)
            if args.record:
                relative = publish_comparison(
                    root, "runs", args.before, args.after, comparison
                )
                state = dict(read_state(root))
                state["current_run_comparison"] = relative
                write_json(root / "state.json", state)
                print(relative)
            elif args.output:
                write_json(args.output, comparison)
            else:
                sys.stdout.buffer.write(canonical(comparison) + b"\n")
        elif args.command == "show":
            show(root)
        return 0
    except (EvaluationError, OSError, subprocess.SubprocessError) as error:
        print(f"archbird evaluation: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
