"""Schema-2 project configuration planning for Query execution."""

from __future__ import annotations

import json
from typing import Mapping

from .errors import ConfigError
from .native import compile_query_plan_json

def compile_named_query(
    config_json: bytes,
    query_id: str,
    *,
    overrides: Mapping[str, object] | None = None,
) -> dict[str, object]:
    """Compile a named Map-independent QueryPlan."""

    try:
        artifact = json.loads(
            compile_query_plan_json(
                config_json,
                query_id,
                overrides=overrides,
                pretty=False,
            )
        )
        plan = artifact["plan"]
    except (KeyError, RuntimeError, TypeError, UnicodeDecodeError, ValueError) as error:
        raise ConfigError(f"invalid native QueryPlan: {error}") from error
    if not isinstance(plan, dict):
        raise ConfigError("native QueryPlan has an invalid plan")
    return plan


def compile_ad_hoc_query(
    options: Mapping[str, object],
) -> dict[str, object]:
    """Compile an ad-hoc Map-independent QueryPlan."""

    try:
        artifact = json.loads(
            compile_query_plan_json(
                b"",
                "",
                overrides=options,
                pretty=False,
            )
        )
        plan = artifact["plan"]
    except (KeyError, RuntimeError, TypeError, UnicodeDecodeError, ValueError) as error:
        raise ConfigError(f"invalid native ad-hoc QueryPlan: {error}") from error
    if not isinstance(plan, dict):
        raise ConfigError("native ad-hoc QueryPlan has an invalid plan")
    return plan
