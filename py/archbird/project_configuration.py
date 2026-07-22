"""Schema-2 project configuration planning for Query execution."""

from __future__ import annotations

import json
from typing import Mapping

from . import _native
from .errors import ConfigError


def _canonical(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def compile_named_query(
    config_json: bytes,
    query_id: str,
    map_json: bytes,
    *,
    overrides: Mapping[str, object] | None = None,
    resolution_json: bytes = b"",
) -> tuple[
    dict[str, object], dict[str, object], list[dict[str, object]]
]:
    """Compile a named Query through the native shared projection planner."""

    try:
        artifact = json.loads(
            _native.query_plan_compile(
                config_json,
                map_json,
                query_id,
                resolution_json=resolution_json,
                overrides_json=_canonical(dict(overrides or {})),
                pretty=False,
            )
        )
        request = artifact["request"]
        plan = artifact["plan"]
        projection_results = artifact["projection_results"]
    except (KeyError, RuntimeError, TypeError, UnicodeDecodeError, ValueError) as error:
        raise ConfigError(f"invalid native QueryPlan: {error}") from error
    if not isinstance(request, dict) or not isinstance(plan, dict):
        raise ConfigError("native QueryPlan has invalid request or plan")
    if not isinstance(projection_results, list) or not all(
        isinstance(row, dict) for row in projection_results
    ):
        raise ConfigError("native QueryPlan has invalid projection results")
    return request, plan, projection_results


def compile_ad_hoc_query(
    map_json: bytes,
    options: Mapping[str, object],
    *,
    resolution_json: bytes = b"",
) -> tuple[
    dict[str, object], dict[str, object], list[dict[str, object]]
]:
    """Compile an ad-hoc Query through the shared projection planner."""

    try:
        artifact = json.loads(
            _native.query_plan_compile(
                b"",
                map_json,
                "",
                resolution_json=resolution_json,
                overrides_json=_canonical(dict(options)),
                pretty=False,
            )
        )
        request = artifact["request"]
        plan = artifact["plan"]
        projection_results = artifact["projection_results"]
    except (KeyError, RuntimeError, TypeError, UnicodeDecodeError, ValueError) as error:
        raise ConfigError(f"invalid native ad-hoc QueryPlan: {error}") from error
    if not isinstance(request, dict) or not isinstance(plan, dict):
        raise ConfigError("native ad-hoc QueryPlan has invalid request or plan")
    if not isinstance(projection_results, list) or not all(
        isinstance(row, dict) for row in projection_results
    ):
        raise ConfigError("native ad-hoc QueryPlan has invalid projection results")
    return request, plan, projection_results
