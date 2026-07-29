"""Deterministic architecture evidence, verification, planning, and action."""

from __future__ import annotations

import hashlib
from functools import lru_cache
from pathlib import Path
from typing import Any

from .schema import read_schema, schema_names


__version__ = "0.0.1"


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
    "compile_project_configuration",
    "compile_query_plan_json",
    "compile_test_observations",
    "diff_maps_json",
    "evaluate_constraints_json",
    "evaluate_projection_json",
    "export_graph",
    "export_okf_bundle",
    "freeze_constraints_json",
    "publish_okf_bundle",
    "query_map_markdown",
    "query_map_json",
    "render_map_markdown",
    "render_source_markdown",
    "resolve_discovery",
    "validate_test_symbol_observations",
    "write_okf_bundle",
)

_PLAN_ACT_EXPORTS = (
    "apply_plan",
    "generate_plan",
    "inspect_ast_grep_executable",
    "materialize_ast_grep_operations",
    "preview_plan",
)


def __getattr__(name: str) -> Any:
    """Lazily expose the native host without hiding package metadata."""

    if name in _PLAN_ACT_EXPORTS:
        if name == "generate_plan":
            from .planning import generate_plan

            return generate_plan
        if name in {
            "inspect_ast_grep_executable",
            "materialize_ast_grep_operations",
        }:
            from .adapters.ast_grep import (
                inspect_ast_grep_executable,
                materialize_ast_grep_operations,
            )

            return {
                "inspect_ast_grep_executable": inspect_ast_grep_executable,
                "materialize_ast_grep_operations": materialize_ast_grep_operations,
            }[name]
        from .acting import apply_plan, preview_plan

        return {"apply_plan": apply_plan, "preview_plan": preview_plan}[name]
    if name not in _NATIVE_EXPORTS:
        raise AttributeError(name)
    from . import native

    return getattr(native, name)


__all__ = [
    "__version__",
    "implementation_digest",
    "read_schema",
    "schema_names",
    *_PLAN_ACT_EXPORTS,
    *_NATIVE_EXPORTS,
]
