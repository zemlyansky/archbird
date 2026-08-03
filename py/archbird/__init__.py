"""Deterministic architecture evidence, verification, planning, and action."""

from __future__ import annotations

import hashlib
from functools import lru_cache
from pathlib import Path
from typing import Any

from .schema import read_schema, schema_names


__version__ = "0.0.2"


@lru_cache(maxsize=1)
def implementation_digest() -> str:
    """Hash the installed engine source without Git, mtimes, or absolute paths."""

    digest = hashlib.sha256()
    root = Path(__file__).resolve().parent
    paths = sorted(
        root.rglob("*.py"), key=lambda item: item.relative_to(root).as_posix()
    )
    for path in paths:
        relative = path.relative_to(root).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
        digest.update(b"\0")
    return digest.hexdigest()


_NATIVE_EXPORTS = (
    "Project",
    "PATTERN_CONTRACT",
    "PATTERN_CONTRACT_VERSION",
    "PATTERN_ENGINE",
    "PATTERN_OPTIONS",
    "PATTERN_UNICODE",
    "Source",
    "Workspace",
    "analyze_workspace_json",
    "analyze_okf_source",
    "audit_map_freshness",
    "accept_act_json",
    "compile_plan_json",
    "compile_project_configuration",
    "compile_query_plan_json",
    "compile_test_observations",
    "diff_maps_json",
    "evaluate_constraints_json",
    "evaluate_projection_json",
    "export_graph",
    "export_okf_bundle",
    "freeze_constraints_json",
    "materialize_act_json",
    "act_source_requirements",
    "plan_source_requirements",
    "preflight_act_apply",
    "publish_okf_bundle",
    "path_map_json",
    "path_map_markdown",
    "query_map_markdown",
    "query_map_json",
    "render_map_markdown",
    "render_plan_markdown",
    "render_source_markdown",
    "resolve_discovery",
    "validate_act",
    "validate_plan",
    "validate_test_symbol_observations",
    "write_okf_bundle",
)

_ADAPTER_EXPORTS = (
    "apply_accepted_act",
    "inspect_ast_grep_executable",
    "materialize_ast_grep_operations",
    "observe_act_sources",
    "observe_plan_sources",
    "run_act_gates",
    "act_overlay",
    "render_act",
)


def __getattr__(name: str) -> Any:
    """Lazily expose the native host without hiding package metadata."""

    if name in _ADAPTER_EXPORTS:
        if name in {
            "apply_accepted_act",
            "observe_act_sources",
            "observe_plan_sources",
            "run_act_gates",
            "act_overlay",
            "render_act",
        }:
            from .act_transport import (
                apply_accepted_act,
                observe_act_sources,
                observe_plan_sources,
                run_act_gates,
                act_overlay,
                render_act,
            )

            return {
                "apply_accepted_act": apply_accepted_act,
                "observe_act_sources": observe_act_sources,
                "observe_plan_sources": observe_plan_sources,
                "run_act_gates": run_act_gates,
                "act_overlay": act_overlay,
                "render_act": render_act,
            }[name]
        from .adapters.ast_grep import (
            inspect_ast_grep_executable,
            materialize_ast_grep_operations,
        )

        return {
            "inspect_ast_grep_executable": inspect_ast_grep_executable,
            "materialize_ast_grep_operations": materialize_ast_grep_operations,
        }[name]
    if name not in _NATIVE_EXPORTS:
        raise AttributeError(name)
    from . import native

    return getattr(native, name)


__all__ = [
    "__version__",
    "implementation_digest",
    "read_schema",
    "schema_names",
    *_ADAPTER_EXPORTS,
    *_NATIVE_EXPORTS,
]
