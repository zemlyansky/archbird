"""Command line for deterministic repository maps and architecture constraints."""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Mapping, Optional, Sequence

from . import __version__, implementation_digest
from . import _native
from .errors import ConfigError
from .provider_cache import (
    default_provider_cache_dir,
    default_provider_cache_max_bytes,
)
from .native import (
    DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS,
    Project,
    Source,
    Workspace,
    audit_map_freshness,
    compile_test_observations,
    diff_maps_json,
    _evaluate_constraints_report_with_blocking,
    evaluate_constraints_json,
    export_graph,
    export_okf_bundle,
    freeze_constraints_json,
    analyze_okf_source,
    path_map_json,
    query_map_markdown,
    query_map_json,
    render_map_markdown,
    render_path_markdown,
    render_plan_markdown,
    render_source_markdown,
    resolve_discovery,
)
from .adapters.okf.parser import okf_query_input, parse_okf_bundle
from .act_transport import (
    apply_accepted_act,
    observe_plan_sources,
    act_overlay,
    gate_failure_details,
    render_act,
    run_act_gates,
)
from ._plan_limits import (
    MAX_ACT_BYTES,
    MAX_OPERATION_TEXT_BYTES,
    MAX_PLAN_BYTES,
)
from .project_configuration import (
    compile_named_query,
)


_PORTABLE_PROVIDERS = (
    "lexical:c",
    "lexical:javascript",
    "lexical:python",
    "lexical:r",
    "syntax:tree-sitter:c",
    "syntax:tree-sitter:cpp",
    "syntax:tree-sitter:python",
    "syntax:tree-sitter:javascript",
    "syntax:tree-sitter:typescript",
    "syntax:tree-sitter:tsx",
    "syntax:tree-sitter:r",
    "semantic:scip",
)

_COMMANDS = (
    "config",
    "act",
    "apply",
    "diff",
    "export",
    "freshness",
    "impact",
    "map",
    "mcp",
    "observe",
    "okf",
    "path",
    "plan",
    "query",
    "serve",
    "support",
    "verify",
    "workspace",
)


def _top_level_help() -> str:
    return (
        "usage: archbird COMMAND [OPTIONS]\n"
        "       archbird [ROOT] [MAP OPTIONS]\n\n"
        "Map codebases, query evidence, verify architecture, plan changes, "
        "and act on reviewed plans.\n\n"
        "commands:\n"
        "  map, config, query, impact, path, diff, observe, freshness, workspace\n"
        "  verify, plan, act, export, okf, serve, mcp, support\n\n"
        "Run `archbird COMMAND --help` for command-specific options. With no "
        "command, Archbird maps the current directory; an existing or "
        "path-shaped positional argument is the Map root.\n"
    )


def _support_main(arguments: Sequence[str]) -> int:
    support = argparse.ArgumentParser(
        prog="archbird support",
        description="Report the active frontend, core, and evidence providers.",
    )
    support.add_argument("--pretty", action="store_true", help="pretty JSON")
    args = support.parse_args(arguments)
    report = {
        "core_implementation_sha256": _native.IMPLEMENTATION_SHA256,
        "engine": {"kind": "native", "source": "python-abi"},
        "frontend_implementation_sha256": implementation_digest(),
        "native_abi_version": _native.NATIVE_ABI_VERSION,
        "pattern": {
            "contract": _native.PATTERN_CONTRACT,
            "contract_version": _native.PATTERN_CONTRACT_VERSION,
            "engine": _native.PATTERN_ENGINE,
            "options": _native.PATTERN_OPTIONS,
            "unicode": _native.PATTERN_UNICODE,
        },
        "providers": {
            "host": ["ast:cpython"],
            "portable": list(_PORTABLE_PROVIDERS),
            "precision": {
                "c": "tree-sitter+lexical",
                "cpp": "tree-sitter+lexical",
                "javascript": "tree-sitter+lexical",
                "python": "cpython-ast+tree-sitter+lexical",
                "r": "tree-sitter+lexical",
                "tsx": "tree-sitter+lexical",
                "typescript": "tree-sitter+lexical",
                "vue": "lexical",
            },
        },
        "runtime": {
            "executable": str(Path(sys.executable).resolve()),
            "implementation": platform.python_implementation(),
            "kind": "python",
            "version": platform.python_version(),
        },
        "version": __version__,
    }
    print(
        json.dumps(
            report,
            ensure_ascii=True,
            indent=2 if args.pretty else None,
            separators=None if args.pretty else (",", ":"),
            sort_keys=True,
        )
    )
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird map",
        description="Generate a deterministic architecture map.",
    )
    result.add_argument("root_path", nargs="?", help="repository root (default: .)")
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument(
        "--no-config", action="store_true", help="ignore root project configuration"
    )
    result.add_argument("--root", dest="root_override", help="repository root")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument("-o", "--output", default="-", help="output path or -")
    result.add_argument(
        "--merge-ledger",
        metavar="PATH",
        help=(
            "write compact provider-conflict JSON, including when provider "
            "finalization fails"
        ),
    )
    result.add_argument(
        "--test-symbol-observations",
        action="append",
        default=[],
        metavar="FILE",
        help="attach a strict runner-observed test-to-symbol artifact; repeatable",
    )
    result.add_argument(
        "--format",
        choices=("markdown", "json"),
        default="markdown",
        help="output format",
    )
    result.add_argument(
        "--view",
        choices=("overview", "architecture", "tests", "evidence", "source"),
        default="overview",
        help="human Markdown projection (default: overview)",
    )
    result.add_argument(
        "--group-by",
        choices=("component", "directory", "language", "layer", "none"),
        default="",
        help="override the view's graph grouping",
    )
    result.add_argument(
        "--level",
        choices=("component", "file", "symbol"),
        default="",
        help="override the view's graph entity level",
    )
    result.add_argument(
        "--relations",
        action="append",
        default=None,
        metavar="KINDS",
        help="override relation families with a comma-separated list; repeatable",
    )
    result.add_argument(
        "--overlay",
        action="append",
        default=None,
        metavar="KINDS",
        help="override Map-derived overlays with a comma-separated list; repeatable",
    )
    result.add_argument(
        "--detail",
        choices=("compact", "standard", "full"),
        default="standard",
        help="amount of evidence in the selected view (default: standard)",
    )
    result.add_argument(
        "--compact",
        action="store_true",
        help="alias for --detail compact",
    )
    result.add_argument(
        "--full",
        action="store_true",
        help="alias for --detail full",
    )
    result.add_argument(
        "--dump",
        action="store_true",
        help="alias for --view source --detail full",
    )
    result.add_argument(
        "--max-chars",
        type=int,
        default=0,
        help="maximum Markdown characters; omit only complete displayed records",
    )
    result.add_argument("--pretty", action="store_true", help="pretty JSON")
    result.add_argument(
        "--check",
        action="store_true",
        help="exit nonzero when the generated map has error diagnostics",
    )
    result.add_argument("--version", action="version", version=__version__)
    return result


def query_parser(command: str, *, default_direction: str) -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog=f"archbird {command}",
        description="Select a deterministic neighborhood from current or saved Map evidence.",
    )
    result.add_argument(
        "query_id",
        nargs="?",
        metavar="QUERY_OR_ROOT",
        help=(
            "named query from archbird.json, or a path-shaped repository root "
            "such as . or ../project"
        ),
    )
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--map", help="saved canonical Map JSON")
    result.add_argument(
        "--resolution", help="configuration-resolution JSON paired with --map"
    )
    result.add_argument("--root", dest="root_override", help="repository root (default: .)")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument(
        "--test-symbol-observations",
        action="append",
        default=[],
        metavar="FILE",
        help="attach a strict runner-observed test-to-symbol artifact; repeatable",
    )
    result.add_argument("--focus", action="append", default=[])
    result.add_argument("--path", action="append", default=[])
    result.add_argument(
        "--symbol",
        action="append",
        default=[],
        metavar="[PATH:]PATTERN",
        help=(
            "symbol pattern, or repository-relative PATH:PATTERN for "
            "conjunctive file scoping; repeatable selectors are a union"
        ),
    )
    result.add_argument("--component", action="append", default=[])
    result.add_argument("--package", action="append", default=[])
    result.add_argument("--artifact", action="append", default=[])
    result.add_argument(
        "--search",
        action="append",
        default=[],
        metavar="KEYWORDS",
        help=(
            "rank lexical candidate seeds from repository names, paths, "
            "signatures, descriptions, and metadata; does not interpret "
            "natural-language questions; repeatable"
        ),
    )
    result.add_argument(
        "--search-limit",
        type=int,
        default=None,
        help="maximum candidate seeds selected by --search (default: 8)",
    )
    result.add_argument(
        "--git-diff",
        metavar="REVISION",
        help=(
            "seed a change query from `git diff REVISION`; tracked additions, "
            "copies, modifications, and rename destinations become current-Map "
            "seeds while deletions remain explicit change evidence"
        ),
    )
    result.add_argument(
        "--verification-result",
        metavar="PATH",
        help=(
            "overlay exact-path-relevant constraints and findings from one canonical "
            "verification result onto a Markdown changes view"
        ),
    )
    result.add_argument("--depth", type=int)
    result.add_argument("--test-depth", type=int)
    result.add_argument(
        "--context-profile",
        choices=("exact", "change", "architecture", "audit"),
        help="typed Markdown selection policy (default: change)",
    )
    result.add_argument(
        "--route-provenance",
        action="append",
        choices=("derived", "asserted", "observed"),
        default=[],
        help="include one route provenance class; repeatable",
    )
    result.add_argument(
        "--route-confidence",
        action="append",
        choices=("exact", "candidate", "conservative", "unresolved"),
        default=[],
        help="include one route confidence class; repeatable",
    )
    result.add_argument("--max-seed-distance", type=int)
    result.add_argument(
        "--candidate",
        choices=("collapse", "expand", "exclude"),
        help="control candidate route rows",
    )
    result.add_argument(
        "--conservative",
        choices=("collapse", "expand", "exclude"),
        help="control conservative route rows",
    )
    result.add_argument(
        "--context-quota",
        action="append",
        default=[],
        metavar="KIND=N",
        help=(
            "limit files, symbol_calls, symbol_references, or test_matches; "
            "repeatable"
        ),
    )
    result.add_argument(
        "--context-offset",
        action="append",
        default=[],
        metavar="KIND=N",
        help="resume one context kind at a deterministic offset; repeatable",
    )
    result.add_argument(
        "--direction",
        choices=("both", "downstream", "upstream"),
        default=None,
    )
    result.add_argument(
        "--format", choices=("markdown", "json"), default="markdown"
    )
    result.add_argument(
        "--view",
        choices=("focused", "changes", "source"),
        default="focused",
        help="human Markdown projection",
    )
    result.add_argument(
        "--detail",
        choices=("compact", "standard", "full"),
        default="standard",
        help="amount of evidence in the selected view",
    )
    result.add_argument("--compact", action="store_true", help="alias for --detail compact")
    result.add_argument("--full", action="store_true", help="alias for --detail full")
    result.add_argument(
        "--dump",
        action="store_true",
        help="alias for --view source --detail full",
    )
    result.add_argument(
        "--max-chars",
        type=int,
        default=0,
        help="final Markdown character guard after typed context selection",
    )
    result.add_argument("--pretty", action="store_true")
    result.add_argument(
        "--check",
        action="store_true",
        help=(
            "block error diagnostics and saved Maps produced by a different "
            "core implementation"
        ),
    )
    result.add_argument("-o", "--output", default="-")
    return result


def path_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird path",
        description=(
            "Find deterministic evidence-preserving connection paths between "
            "two explicit endpoint sets."
        ),
    )
    result.add_argument("source_pattern", metavar="SOURCE")
    result.add_argument("target_pattern", metavar="TARGET")
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--map", help="saved canonical Map JSON")
    result.add_argument(
        "--resolution", help="configuration-resolution JSON paired with --map"
    )
    result.add_argument("--root", dest="root_override", help="repository root")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument(
        "--test-symbol-observations",
        action="append",
        default=[],
        metavar="FILE",
        help="attach runner-observed test-to-symbol evidence; repeatable",
    )
    result.add_argument(
        "--level", choices=("component", "file", "symbol"), default="file"
    )
    result.add_argument(
        "--source-kind",
        help="source graph entity kind (default: selected level)",
    )
    result.add_argument(
        "--target-kind",
        help="target graph entity kind (default: selected level)",
    )
    result.add_argument(
        "--relation",
        action="append",
        default=[],
        choices=(
            "bridges",
            "builds",
            "calls",
            "declarations",
            "imports",
            "packages",
            "references",
            "tests",
        ),
        help="include one typed relation family; repeatable",
    )
    result.add_argument(
        "--direction",
        choices=("downstream", "upstream", "both"),
        default="downstream",
    )
    result.add_argument("--max-depth", type=int, default=8)
    result.add_argument("--max-paths", type=int, default=8)
    result.add_argument(
        "--format", choices=("markdown", "json"), default="markdown"
    )
    result.add_argument(
        "--max-chars",
        type=int,
        default=0,
        help="maximum Markdown characters",
    )
    result.add_argument("--pretty", action="store_true", help="pretty JSON")
    result.add_argument(
        "--check",
        action="store_true",
        help="require current producer evidence and at least one path witness",
    )
    result.add_argument("-o", "--output", default="-")
    result.set_defaults(root_path=None)
    return result


def freshness_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird freshness",
        description=(
            "Audit a saved Map or Query snapshot against a freshly derived "
            "live repository Map."
        ),
    )
    result.add_argument("root_path", nargs="?", help="repository root (default: .)")
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="live project configuration JSON")
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--root", dest="root_override", help="compatibility root override")
    result.add_argument("--snapshot", required=True, help="saved Map or Query JSON")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument("--pretty", action="store_true")
    result.add_argument("--check", action="store_true")
    result.add_argument("-o", "--output", default="-")
    return result


def _add_discovery_options(result: argparse.ArgumentParser) -> None:
    result.add_argument("--project", help="override canonical project identity")
    result.add_argument(
        "--source",
        action="append",
        default=[],
        metavar="LANGUAGE=GLOB",
        help="add a highest-priority source classification",
    )
    result.add_argument(
        "--only", action="append", default=[], metavar="GLOB", help="restrict selected paths"
    )
    result.add_argument(
        "--exclude", action="append", default=[], metavar="GLOB", help="exclude selected paths"
    )
    result.add_argument(
        "--ignore-file",
        action="append",
        default=[],
        metavar="PATH",
        help="add repository-relative gitignore-syntax rules",
    )
    result.add_argument(
        "--no-ignore",
        action="store_true",
        help="ignore repository .gitignore/.ignore/.archbirdignore files",
    )
    result.add_argument(
        "--no-default-excludes",
        action="store_true",
        help="disable Archbird's versioned VCS/build/cache exclusions",
    )
    result.add_argument(
        "--max-file-bytes",
        type=int,
        help="override the discovery source-read limit",
    )
    result.add_argument(
        "--max-index-bytes",
        type=int,
        help="override the semantic-index read limit",
    )


def _add_python_analysis_options(result: argparse.ArgumentParser) -> None:
    result.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Python analyzer processes; 0 selects automatically",
    )
    result.add_argument(
        "--python-provider-timeout",
        type=float,
        metavar="SECONDS",
        help=(
            "maximum wait for each ordered multiprocess CPython AST source batch "
            f"(default: {DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS:g})"
        ),
    )


def _python_provider_timeout(args: argparse.Namespace) -> float:
    value = getattr(args, "python_provider_timeout", None)
    resolved = (
        DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS
        if value is None
        else value
    )
    if not math.isfinite(resolved) or resolved <= 0:
        raise ValueError("--python-provider-timeout must be finite and positive")
    return resolved


def _add_cache_options(result: argparse.ArgumentParser) -> None:
    result.add_argument(
        "--cache-dir",
        help=(
            "persistent provider cache root "
            "(default: ARCHBIRD_CACHE_DIR, XDG cache, or ~/.cache/archbird)"
        ),
    )
    result.add_argument(
        "--cache-max-bytes",
        type=int,
        help=(
            "maximum persistent provider-cache bytes "
            "(default: ARCHBIRD_CACHE_MAX_BYTES or 1073741824)"
        ),
    )
    result.add_argument(
        "--no-cache",
        action="store_true",
        help="disable persistent per-file provider reuse",
    )


def _add_progress_options(result: argparse.ArgumentParser) -> None:
    result.add_argument(
        "--progress",
        choices=("auto", "always", "never"),
        default="auto",
        help="stderr phase progress: auto for terminals, always, or never",
    )


def _cache_dir(args: argparse.Namespace) -> Optional[Path]:
    if args.no_cache:
        return None
    return (
        Path(args.cache_dir).expanduser()
        if args.cache_dir
        else default_provider_cache_dir()
    )


def _cache_max_bytes(args: argparse.Namespace) -> int:
    value = (
        args.cache_max_bytes
        if args.cache_max_bytes is not None
        else default_provider_cache_max_bytes()
    )
    if value <= 0 or value > (1 << 53) - 1:
        raise ValueError("--cache-max-bytes must be a positive safe integer")
    return value


def _warn_cache_stats(stats: Mapping[str, int]) -> None:
    if stats.get("no_space", 0):
        print(
            "archbird: warning: provider-cache write failed because storage "
            "is full; analysis remains valid. Use --cache-dir, increase "
            "--cache-max-bytes, or use --no-cache.",
            file=sys.stderr,
        )
    if stats.get("skipped", 0):
        print(
            "archbird: warning: provider-cache entries exceeded the configured "
            "budget and were not stored; analysis remains valid. Increase "
            "--cache-max-bytes or use --no-cache.",
            file=sys.stderr,
        )


def _warn_map_cache_stats(stats: Mapping[str, int]) -> None:
    if stats.get("no_space", 0):
        print(
            "archbird: warning: canonical Map cache write failed because "
            "storage is full; analysis remains valid.",
            file=sys.stderr,
        )
    if stats.get("skipped", 0):
        print(
            "archbird: warning: canonical Map exceeded the configured cache "
            "budget and was not stored; analysis remains valid.",
            file=sys.stderr,
        )


def config_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird config",
        description="Inspect or materialize deterministic discovery configuration.",
    )
    commands = result.add_subparsers(dest="command", required=True)
    for name in ("show", "init"):
        child = commands.add_parser(name)
        child.add_argument("root_path", nargs="?", help="repository root (default: .)")
        source = child.add_mutually_exclusive_group()
        source.add_argument("-c", "--config", help="project configuration JSON")
        source.add_argument("--no-config", action="store_true")
        child.add_argument("--root", dest="root_override")
        _add_discovery_options(child)
        child.add_argument("--format", choices=("json",), default="json")
        child.add_argument("--pretty", action="store_true")
        child.add_argument("--check", action="store_true")
        child.add_argument("-o", "--output", default="-" if name == "show" else "archbird.json")
        if name == "init":
            child.add_argument("--force", action="store_true")
    return result


def serve_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird serve",
        description=(
            "Serve the offline visualization on loopback and retain the last "
            "good Map while repository changes are analyzed."
        ),
    )
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="live project configuration JSON")
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--root", dest="root_override")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    result.add_argument("--host", default="127.0.0.1", choices=("127.0.0.1", "::1"))
    result.add_argument("--port", type=int, default=4177)
    result.add_argument("--app", help=argparse.SUPPRESS)
    return result


def mcp_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird mcp",
        description=(
            "Expose bounded Map, projection, Query, source, Verify, and Diff "
            "tools over MCP stdio."
        ),
    )
    source = result.add_mutually_exclusive_group()
    source.add_argument(
        "-c",
        "--config",
        help="project configuration file; stdin is reserved for MCP",
    )
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--root", dest="root_override")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    return result


def diff_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird diff",
        description="Compare two canonical Map JSON artifacts structurally.",
    )
    result.add_argument("--before", required=True)
    result.add_argument("--after", required=True)
    result.add_argument("--format", choices=("json",), default="json")
    result.add_argument("--pretty", action="store_true")
    result.add_argument(
        "--check",
        nargs="?",
        const="public-api,bridges,parity,tests,architecture",
        help="fail on a comma-separated set of structural risk categories",
    )
    result.add_argument("-o", "--output", default="-")
    return result


def observe_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird observe",
        description=(
            "Convert project-owned per-test coverage reports into exact "
            "test-to-symbol observations without running the project."
        ),
    )
    result.add_argument("root_path", nargs="?", help="repository root (default: .)")
    result.add_argument("--map", required=True, help="canonical Map JSON")
    result.add_argument(
        "--request", required=True, help="coverage observation request JSON"
    )
    result.add_argument("-o", "--output", default="-", help="output artifact or -")
    return result


def workspace_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird workspace",
        description="Map package and import routes across configured projects.",
    )
    result.add_argument("-c", "--config", required=True)
    result.add_argument("--format", choices=("json",), default="json")
    result.add_argument("--pretty", action="store_true")
    _add_python_analysis_options(result)
    _add_cache_options(result)
    result.add_argument("--check", action="store_true")
    result.add_argument("-o", "--output", default="-")
    return result


def verification_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird verify",
        description=(
            "Evaluate all or selected architecture constraints from archbird.json."
        ),
    )
    result.add_argument(
        "constraint_ids",
        nargs="*",
        metavar="CONSTRAINT",
        help="constraint IDs; omit to evaluate the complete configured policy",
    )
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument(
        "--no-config",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    result.add_argument("--map", help="saved canonical Map JSON")
    result.add_argument(
        "--resolution",
        help="configuration-resolution JSON paired with --map",
    )
    result.add_argument("--root", dest="root_override", help="repository root (default: .)")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument(
        "--baseline",
        help="classify findings against an explicit frozen baseline",
    )
    result.add_argument(
        "--policy-date",
        metavar="YYYY-MM-DD",
        help=(
            "date used to evaluate expiring waivers; defaults to the current "
            "UTC date only when an expiring waiver is configured"
        ),
    )
    result.add_argument(
        "--observation",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply a reviewed observation artifact for a constraint operand",
    )
    result.add_argument(
        "--map-input",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply an additional saved Map for a projection operand",
    )
    result.add_argument(
        "--resolution-input",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply configuration resolution for an additional saved Map",
    )
    result.add_argument(
        "--freeze",
        help="write/update a reviewed violation and coverage-ratchet baseline",
    )
    result.add_argument("--freeze-owner", help="owner recorded by --freeze")
    result.add_argument(
        "--freeze-rationale", help="review rationale recorded by --freeze"
    )
    result.add_argument("-o", "--output", default="-", help="output path or -")
    result.add_argument(
        "--format",
        choices=("json", "markdown", "sarif", "junit"),
        default="markdown",
        help="output format",
    )
    result.add_argument(
        "--full",
        action="store_true",
        help="include every finding in Markdown",
    )
    result.add_argument(
        "--max-findings",
        type=int,
        default=None,
        help="maximum findings in compact Markdown (default: 200)",
    )
    result.add_argument("--pretty", action="store_true", help="pretty JSON")
    result.add_argument(
        "--check",
        action="store_true",
        help="exit nonzero when verification contains blocking evidence",
    )
    result.add_argument("--version", action="version", version=__version__)
    return result


def plan_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird plan",
        description=(
            "Derive an editable source-locked Plan from current constraint issues."
        ),
    )
    result.add_argument(
        "constraint_ids",
        nargs="*",
        metavar="CONSTRAINT",
        help="constraint IDs to plan; omit to plan every current issue",
    )
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument(
        "--no-config",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    result.add_argument("--map", help="saved canonical Map JSON")
    result.add_argument(
        "--before-map",
        help=(
            "previous canonical Map used to derive residual work from an "
            "observed repository change"
        ),
    )
    result.add_argument(
        "--git-diff",
        metavar="REVISION",
        help=(
            "derive the before Map from one Git commit and plan residual work "
            "against the current working tree"
        ),
    )
    result.add_argument(
        "--resolution",
        help="configuration-resolution JSON paired with --map",
    )
    result.add_argument("--root", dest="root_override", help="repository root (default: .)")
    _add_discovery_options(result)
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument(
        "--baseline",
        help="classify issues against an explicit frozen baseline",
    )
    result.add_argument(
        "--policy-date",
        metavar="YYYY-MM-DD",
        help="date used to evaluate expiring waivers",
    )
    result.add_argument(
        "--observation",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply a reviewed observation artifact",
    )
    result.add_argument(
        "--map-input",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply an additional saved Map for a constraint operand",
    )
    result.add_argument(
        "--resolution-input",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply configuration resolution for an additional saved Map",
    )
    result.add_argument(
        "--objective",
        help="replace the derived Plan objective with reviewed text",
    )
    result.add_argument(
        "--rename",
        action="append",
        default=[],
        metavar="OLD=NEW",
        help=(
            "review one identity migration that closes matching symbol or "
            "provider-surface issues in a selected constraint"
        ),
    )
    result.add_argument(
        "--redirect",
        action="append",
        default=[],
        metavar="FROM=TO",
        help=(
            "review one dependency replacement for a selected edge "
            "constraint"
        ),
    )
    result.add_argument(
        "--format",
        choices=("json", "markdown"),
        default="json",
        help="canonical editable Plan JSON or a native Markdown task packet",
    )
    result.add_argument("--pretty", action="store_true", help="pretty JSON")
    result.add_argument("-o", "--output", default="-")
    return result


def act_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird act",
        description=(
            "Materialize a source-locked Plan in isolation and emit an accepted "
            "Act after a fresh Map and Verify."
        ),
    )
    result.add_argument("plan", help="editable Plan JSON")
    result.add_argument("-c", "--config", help="project configuration JSON")
    result.add_argument("--root", dest="root_override", help="repository root (default: .)")
    _add_python_analysis_options(result)
    _add_cache_options(result)
    _add_progress_options(result)
    result.add_argument(
        "--baseline",
        help="classify acceptance against an explicit frozen baseline",
    )
    result.add_argument(
        "--policy-date",
        metavar="YYYY-MM-DD",
        help="date used to evaluate expiring waivers",
    )
    result.add_argument(
        "--observation",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply a reviewed observation artifact for acceptance",
    )
    result.add_argument(
        "--map-input",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply an additional saved Map for acceptance",
    )
    result.add_argument(
        "--resolution-input",
        action="append",
        default=[],
        metavar="ID=PATH",
        help="supply configuration resolution for an additional saved Map",
    )
    result.add_argument(
        "--submit",
        action="append",
        default=[],
        metavar="ITEM=FILE",
        help=(
            "submit reviewed full-file content for an exact unresolved Plan "
            "item; repeat for coordinated items"
        ),
    )
    result.add_argument(
        "--format",
        choices=("markdown", "json", "patch"),
        default="markdown",
    )
    result.add_argument("--pretty", action="store_true")
    result.add_argument("-o", "--output", default="-")
    result.set_defaults(no_config=False, root_path=None)
    return result


def apply_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird apply",
        description=(
            "Replay the exact bytes of an accepted Act after source-preimage "
            "and destination checks."
        ),
    )
    result.add_argument("act", help="accepted Act JSON")
    result.add_argument(
        "--root", dest="root_override", help="repository root (default: .)"
    )
    return result


def export_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird export",
        description="Project a canonical saved Map or Query into an interchange format.",
    )
    result.add_argument("format", choices=("graphml", "json", "mermaid", "okf"))
    result.add_argument(
        "--map", required=True, help="canonical saved Map or Query JSON"
    )
    result.add_argument("--verification", help="canonical verification JSON")
    result.add_argument(
        "--replace",
        action="store_true",
        help="replace only an intact Archbird-generated OKF directory",
    )
    result.add_argument(
        "--view",
        choices=("components", "files", "symbols"),
        default="components",
    )
    result.add_argument(
        "--direction", choices=("BT", "LR", "RL", "TB"), default="LR"
    )
    result.add_argument("--max-nodes", type=int, default=200)
    result.add_argument("--max-edge-names", type=int, default=3)
    result.add_argument("-o", "--output", default="-")
    return result


def okf_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird okf",
        description="Validate and query OKF metadata without treating prose as constraints.",
    )
    commands = result.add_subparsers(dest="command", required=True)
    for name in ("validate", "index"):
        child = commands.add_parser(name)
        child.add_argument("bundle")
        child.add_argument("--format", choices=("markdown", "json"), default="markdown")
        child.add_argument("-o", "--output", default="-")
        if name == "index":
            child.add_argument("--check", action="store_true")
    query = commands.add_parser("query")
    query.add_argument("bundle")
    query.add_argument("--concept", action="append", default=[])
    query.add_argument("--type", dest="types", action="append", default=[])
    query.add_argument("--tag", action="append", default=[])
    query.add_argument("--text", action="append", default=[])
    query.add_argument("--requirement", action="append", default=[])
    query.add_argument("--format", choices=("markdown", "json"), default="markdown")
    query.add_argument("--check", action="store_true")
    query.add_argument("-o", "--output", default="-")
    return result


def _write(encoded: bytes, output: str) -> None:
    value = encoded if encoded.endswith(b"\n") else encoded + b"\n"
    if output == "-":
        sys.stdout.buffer.write(value)
    else:
        Path(output).write_bytes(value)


def _repository_artifact_path(
    repository: Path, locator: str
) -> Optional[str]:
    """Return an in-repository CLI artifact path for transient discovery."""

    if locator == "-":
        return None
    candidate = Path(locator).resolve()
    try:
        relative = candidate.relative_to(repository).as_posix()
    except ValueError:
        return None
    return relative if relative and relative != "." else None


def _read_bounded_regular(
    path: Path, maximum: int, description: str
) -> bytes:
    before = path.lstat()
    if stat.S_ISLNK(before.st_mode) or not stat.S_ISREG(before.st_mode):
        raise ValueError(f"{description} must be a regular non-symlink file")
    if before.st_size > maximum:
        raise ValueError(f"{description} exceeds the {maximum}-byte limit")
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or opened.st_dev != before.st_dev
            or opened.st_ino != before.st_ino
        ):
            raise OSError(f"{description} changed while opening")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(1024 * 1024, maximum + 1))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > maximum:
                raise ValueError(
                    f"{description} exceeds the {maximum}-byte limit"
                )
        after = os.fstat(descriptor)
        if (
            after.st_dev != opened.st_dev
            or after.st_ino != opened.st_ino
            or after.st_size != opened.st_size
            or after.st_mtime_ns != opened.st_mtime_ns
        ):
            raise OSError(f"{description} changed while reading")
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _act_executor_submissions(
    values: Sequence[str],
) -> tuple[bytes, tuple[Path, ...]]:
    rows: list[dict[str, str]] = []
    files: list[Path] = []
    seen: set[str] = set()
    for value in values:
        item_id, separator, raw_path = value.partition("=")
        if not separator or not item_id or not raw_path:
            raise ValueError("--submit expects ITEM=FILE")
        if item_id in seen:
            raise ValueError(f"--submit repeats Plan item {item_id}")
        replacement_path = Path(raw_path).resolve()
        content = _read_bounded_regular(
            replacement_path,
            MAX_OPERATION_TEXT_BYTES,
            "executor submission",
        )
        rows.append(
            {
                "content_base64": base64.b64encode(content).decode("ascii"),
                "item_id": item_id,
                "kind": "write_file",
            }
        )
        files.append(replacement_path)
        seen.add(item_id)
    if not rows:
        return b"", ()
    rows.sort(key=lambda row: row["item_id"])
    encoded = json.dumps(
        {"items": rows},
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return _native.json_canonicalize(encoded), tuple(files)


def _write_project_map(project: Project, output: str, *, pretty: bool) -> None:
    if output == "-":
        project.write_map_json(sys.stdout.buffer.write, pretty=pretty)
        sys.stdout.buffer.write(b"\n")
        return
    destination = Path(output)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            project.write_map_json(stream.write, pretty=pretty)
            stream.write(b"\n")
        if destination.exists():
            temporary.chmod(stat.S_IMODE(destination.stat().st_mode))
        else:
            current_umask = os.umask(0)
            os.umask(current_umask)
            temporary.chmod(0o666 & ~current_umask)
        os.replace(temporary, destination)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


class _Progress:
    def __init__(self, mode: str) -> None:
        self.mode = mode
        self.interactive = mode == "auto" and sys.stderr.isatty()
        self.enabled = mode == "always" or self.interactive
        self.started = time.monotonic()
        self.last_update = self.started
        self.last_message = ""
        self.last_width = 0
        self.visible = False

    def emit(self, event: Mapping[str, object]) -> None:
        if not self.enabled:
            return
        phase = str(event.get("phase", "working"))
        state = str(event.get("state", ""))
        provider = str(event.get("provider", ""))
        completed = event.get("completed")
        total = event.get("total")
        now = time.monotonic()
        if phase == "providers":
            if total == 0:
                return
            if state == "progress" and completed != total and now - self.last_update < 1:
                return
            if isinstance(completed, int) and isinstance(total, int):
                percent = round(completed * 100 / total) if total else 100
                detail = f"{provider} {completed}/{total} files ({percent}%)"
            elif state == "start":
                detail = f"{provider} started"
            else:
                detail = f"{provider} complete"
        elif phase == "discovery":
            detail = "scanning repository"
        elif phase == "selected":
            detail = f"{event.get('files', 0)} files"
        elif phase == "joining":
            detail = "merging normalized facts" if state == "start" else "fact graph ready"
        elif phase == "rendering":
            detail = str(event.get("artifact", "output"))
        elif phase == "complete":
            detail = "done"
        else:
            detail = state or "working"
        message = f"archbird [{now - self.started:.1f}s] {phase}: {detail}"
        if message == self.last_message:
            return
        if self.interactive:
            if now - self.started < 0.75:
                self.last_message = message
                return
            if self.visible and now - self.last_update < 0.2 and phase not in {
                "complete",
                "rendering",
            }:
                self.last_message = message
                return
            padding = " " * max(0, self.last_width - len(message))
            sys.stderr.write(f"\r{message}{padding}")
            sys.stderr.flush()
            self.last_width = len(message)
            self.visible = True
        else:
            print(message, file=sys.stderr, flush=True)
        self.last_message = message
        self.last_update = now

    def finish(self) -> None:
        if not self.enabled:
            return
        elapsed = time.monotonic() - self.started
        if self.interactive:
            if self.visible:
                message = f"archbird [{elapsed:.1f}s] complete"
                padding = " " * max(0, self.last_width - len(message))
                sys.stderr.write(f"\r{message}{padding}\n")
                sys.stderr.flush()
                self.visible = False
            return
        self.emit({"phase": "complete"})

    def clear(self) -> None:
        if self.interactive and self.visible:
            sys.stderr.write("\r" + " " * self.last_width + "\r")
            sys.stderr.flush()
            self.visible = False


def _named_json_documents(
    values: Sequence[str], *, option: str
) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if not separator or not name or not raw_path:
            raise ConfigError(f"{option}: expected ID=PATH, got {value!r}")
        if name in result:
            raise ConfigError(f"{option}: duplicate id {name!r}")
        document = json.loads(Path(raw_path).read_bytes())
        if not isinstance(document, dict):
            raise ConfigError(f"{option}: {raw_path} must contain a JSON object")
        result[name] = document
    return result


def _has_error_diagnostics(document: object) -> bool:
    return isinstance(document, dict) and any(
        isinstance(row, dict) and row.get("severity") == "error"
        for row in document.get("diagnostics", [])
    )


def _validate_project_configuration(config_json: bytes) -> None:
    try:
        document = json.loads(config_json)
    except (UnicodeDecodeError, ValueError) as error:
        raise ConfigError(f"invalid project configuration JSON: {error}") from error
    if not isinstance(document, dict):
        raise ConfigError("project configuration must be an object")
    if "root" in document:
        raise ConfigError("archbird.json does not allow root; use --root")


def _repository_inputs(args: argparse.Namespace) -> tuple[Path, bytes, Optional[Path]]:
    root_path = getattr(args, "root_path", None)
    positional = Path(root_path).resolve() if root_path else None
    override = Path(args.root_override).resolve() if args.root_override else None
    if positional is not None and override is not None and positional != override:
        raise ConfigError("positional ROOT and --root select different directories")
    selected = positional or override or Path.cwd().resolve()
    config_path: Optional[Path] = None
    config_json = b""
    if args.config:
        if args.config == "-":
            config_json = sys.stdin.buffer.read()
        else:
            config_path = Path(args.config).resolve()
            try:
                config_json = config_path.read_bytes()
            except OSError as error:
                raise ConfigError(
                    f"cannot read configuration: {config_path}: {error}"
                ) from error
        _validate_project_configuration(config_json)
    elif not args.no_config:
        candidates = []
        for candidate in (selected / "archbird.json",):
            try:
                metadata = candidate.lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISREG(metadata.st_mode):
                candidates.append(candidate)
        if candidates:
            config_path = candidates[0].resolve()
            config_json = config_path.read_bytes()
            _validate_project_configuration(config_json)
    if not selected.is_dir():
        raise ConfigError(f"root is not a directory: {selected}")
    return selected, config_json, config_path


def _query_positional_is_root(value: Optional[str]) -> bool:
    if not value:
        return False
    separators = tuple(
        separator for separator in (os.sep, os.altsep) if separator
    )
    return (
        value in (".", "..")
        or Path(value).is_absolute()
        or any(separator in value for separator in separators)
    )


def _map_shortcut(arguments: Sequence[str]) -> bool:
    if not arguments or arguments[0].startswith("-"):
        return True
    value = arguments[0]
    return _query_positional_is_root(value) or Path(value).is_dir()


def _normalize_query_positional(args: argparse.Namespace) -> None:
    if not _query_positional_is_root(args.query_id):
        return
    args.root_path = args.query_id
    args.query_id = None


def _has_discovery_overrides(args: argparse.Namespace) -> bool:
    return bool(
        args.project
        or args.source
        or args.only
        or args.exclude
        or args.ignore_file
        or args.no_ignore
        or args.no_default_excludes
        or args.max_file_bytes is not None
        or args.max_index_bytes is not None
        or args.no_config
        or args.cache_dir
        or args.cache_max_bytes is not None
        or args.no_cache
    )


_GIT_CHANGE_STATUS = {
    "A": "added",
    "B": "broken-pair",
    "C": "copied",
    "D": "deleted",
    "M": "modified",
    "R": "renamed",
    "T": "type-changed",
    "U": "unmerged",
    "X": "unknown",
}


def _git_change_set(repository: Path, revision: str) -> dict[str, object]:
    if (
        not revision
        or revision != revision.strip()
        or revision.startswith("-")
        or "\0" in revision
        or "\n" in revision
        or "\r" in revision
    ):
        raise ValueError("--git-diff requires one safe Git revision or range")
    environment = os.environ.copy()
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    completed = subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "diff",
            "--no-ext-diff",
            "--no-textconv",
            "--name-status",
            "-z",
            "--find-renames",
            revision,
            "--",
        ],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(f"git diff failed for {revision!r}: {detail}")
    if completed.stdout and not completed.stdout.endswith(b"\0"):
        raise ValueError("git diff emitted unterminated name-status evidence")
    fields = completed.stdout.split(b"\0")
    if fields:
        fields.pop()
    entries: list[dict[str, str]] = []
    index = 0
    while index < len(fields):
        try:
            raw_status = fields[index].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("git diff emitted a non-ASCII status") from error
        index += 1
        code = raw_status[:1]
        status = _GIT_CHANGE_STATUS.get(code)
        path_count = 2 if code in {"C", "R"} else 1
        if status is None or index + path_count > len(fields):
            raise ValueError("git diff emitted malformed name-status evidence")
        try:
            paths = [
                fields[index + offset].decode("utf-8")
                for offset in range(path_count)
            ]
        except UnicodeDecodeError as error:
            raise ValueError(
                "git diff path is not UTF-8 and cannot enter canonical evidence"
            ) from error
        index += path_count
        entry = {"path": paths[-1], "status": status}
        if path_count == 2:
            entry["previous_path"] = paths[0]
        entries.append(entry)
    if not entries:
        raise ValueError(f"git diff {revision!r} contains no changed paths")
    entries.sort(
        key=lambda row: (row["path"], row["status"], row.get("previous_path", ""))
    )
    if any(entries[index] == entries[index - 1] for index in range(1, len(entries))):
        raise ValueError("git diff emitted duplicate change entries")
    return {
        "entries": entries,
        "source": {"identity": revision, "kind": "git-diff"},
    }


def _git_command(
    repository: Path,
    arguments: Sequence[str],
    *,
    description: str,
) -> bytes:
    environment = os.environ.copy()
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    completed = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(f"{description} failed: {detail}")
    return completed.stdout


def _git_commit(repository: Path, revision: str) -> str:
    if (
        not revision
        or revision != revision.strip()
        or revision.startswith("-")
        or "\0" in revision
        or "\n" in revision
        or "\r" in revision
    ):
        raise ValueError("--git-diff requires one safe Git commit")
    encoded = _git_command(
        repository,
        ["rev-parse", "--verify", f"{revision}^{{commit}}"],
        description=f"git revision {revision!r}",
    )
    try:
        commit = encoded.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise ValueError("git rev-parse emitted a non-ASCII object id") from error
    if len(commit) not in {40, 64} or any(
        character not in "0123456789abcdef" for character in commit
    ):
        raise ValueError("git rev-parse emitted an invalid commit object id")
    return commit


def _git_project_prefix(repository: Path) -> str:
    encoded = _git_command(
        repository,
        ["rev-parse", "--show-prefix"],
        description="locate project root within Git repository",
    )
    try:
        prefix = encoded.decode("utf-8").rstrip("\n")
    except UnicodeDecodeError as error:
        raise ValueError("Git project prefix must be UTF-8") from error
    if prefix and (
        not prefix.endswith("/")
        or prefix.startswith("/")
        or "\\" in prefix
        or any(part in {"", ".", ".."} for part in prefix[:-1].split("/"))
    ):
        raise ValueError("git rev-parse emitted an unsafe project prefix")
    return prefix


def _git_write_blob(
    repository: Path, object_id: str, target: Path, relative: str
) -> None:
    environment = os.environ.copy()
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    with target.open("wb") as output:
        completed = subprocess.run(
            ["git", "-C", str(repository), "cat-file", "blob", object_id],
            env=environment,
            stdout=output,
            stderr=subprocess.PIPE,
            check=False,
        )
    if completed.returncode:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(f"read Git blob for {relative!r} failed: {detail}")


def _git_materialize_tree(
    repository: Path,
    commit: str,
    destination: Path,
    *,
    project_prefix: str = "",
    depth: int = 0,
) -> None:
    if depth > 32:
        raise ValueError("Git submodule nesting exceeds 32 levels")
    inventory_arguments = ["ls-tree", "-rz", "--full-tree", commit]
    if project_prefix:
        inventory_arguments.extend(["--", f":(literal){project_prefix[:-1]}"])
    inventory = _git_command(
        repository,
        inventory_arguments,
        description=f"Git tree inventory for {commit}",
    )
    if inventory and not inventory.endswith(b"\0"):
        raise ValueError("git ls-tree emitted an unterminated record")
    for record in inventory.split(b"\0"):
        if not record:
            continue
        metadata, separator, raw_path = record.partition(b"\t")
        fields = metadata.split(b" ")
        if not separator or len(fields) != 3:
            raise ValueError("git ls-tree emitted a malformed record")
        mode, object_type, raw_object = fields
        try:
            committed_path = raw_path.decode("utf-8")
            object_id = raw_object.decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError(
                "Git snapshot paths and object ids must be UTF-8/ASCII"
            ) from error
        if not committed_path.startswith(project_prefix):
            raise ValueError("Git tree entry is outside the project root")
        relative = committed_path[len(project_prefix) :]
        parts = relative.split("/")
        if (
            not relative
            or relative.startswith("/")
            or "\\" in relative
            or any(part in {"", ".", ".."} for part in parts)
        ):
            raise ValueError(f"Git snapshot contains unsafe path: {relative!r}")
        if len(object_id) not in {40, 64} or any(
            character not in "0123456789abcdef" for character in object_id
        ):
            raise ValueError("git ls-tree emitted an invalid object id")
        target = destination.joinpath(*parts)
        if object_type == b"commit":
            local_submodule = repository.joinpath(*parts)
            git_marker = local_submodule / ".git"
            if (
                not local_submodule.is_dir()
                or local_submodule.is_symlink()
                or not git_marker.exists()
                or git_marker.is_symlink()
            ):
                continue
            _git_command(
                local_submodule,
                ["cat-file", "-e", f"{object_id}^{{commit}}"],
                description=f"resolve Git submodule {relative!r}",
            )
            target.mkdir(parents=True, exist_ok=True)
            _git_materialize_tree(
                local_submodule,
                object_id,
                target,
                depth=depth + 1,
            )
            continue
        if mode == b"120000":
            continue
        if object_type != b"blob" or mode not in {b"100644", b"100755"}:
            raise ValueError(
                f"Git snapshot contains unsupported entry: {relative!r}"
            )
        target.parent.mkdir(parents=True, exist_ok=True)
        _git_write_blob(repository, object_id, target, relative)
        target.chmod(0o755 if mode == b"100755" else 0o644)


@contextmanager
def _git_snapshot(repository: Path, revision: str):
    commit = _git_commit(repository, revision)
    project_prefix = _git_project_prefix(repository)
    temporary_root = default_provider_cache_dir() / "temporary-snapshots"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="git-",
        dir=temporary_root,
    ) as raw:
        snapshot = Path(raw) / (repository.name or "repository")
        snapshot.mkdir()
        _git_materialize_tree(
            repository,
            commit,
            snapshot,
            project_prefix=project_prefix,
        )
        yield snapshot


def _discover_project_from_args(
    args: argparse.Namespace,
    progress: Optional[_Progress] = None,
    *,
    resolved_repository: Optional[Path] = None,
    resolved_config_json: Optional[bytes] = None,
) -> Project:
    if progress is not None:
        progress.emit({"phase": "discovery", "state": "start"})
    if resolved_repository is None or resolved_config_json is None:
        repository, config_json, _ = _repository_inputs(args)
    else:
        repository = resolved_repository
        config_json = resolved_config_json
    current = Project.from_repository(
        repository,
        config=config_json or None,
        project=args.project,
        source=args.source,
        only=args.only,
        exclude=args.exclude,
        ignore=not args.no_ignore,
        ignore_files=args.ignore_file,
        default_excludes=not args.no_default_excludes,
        max_file_bytes=args.max_file_bytes,
        max_index_bytes=args.max_index_bytes,
        _transient_exclude=getattr(args, "_transient_exclude", ()),
        scan=False,
        jobs=args.jobs,
    )
    if progress is not None:
        progress.emit({"phase": "selected", "files": len(current.sources)})
    return current


def _project_from_args(
    args: argparse.Namespace,
    progress: Optional[_Progress] = None,
    *,
    resolved_repository: Optional[Path] = None,
    resolved_config_json: Optional[bytes] = None,
) -> Project:
    current = _discover_project_from_args(
        args,
        progress,
        resolved_repository=resolved_repository,
        resolved_config_json=resolved_config_json,
    )
    ledger_path = getattr(args, "merge_ledger", None)
    if ledger_path and ledger_path == getattr(args, "output", None):
        raise ValueError("--merge-ledger and --output must be different paths")
    try:
        current.scan(
            jobs=args.jobs,
            python_provider_timeout=_python_provider_timeout(args),
            cache_dir=_cache_dir(args),
            cache_max_bytes=_cache_max_bytes(args),
            progress=progress.emit if progress is not None else None,
            map_cache=not bool(
                getattr(args, "test_symbol_observations", ())
            ),
        )
    except Exception:
        if ledger_path:
            _write(current.merge_conflicts_json(pretty=True), ledger_path)
        raise
    _warn_cache_stats(current.cache_stats)
    if ledger_path:
        _write(current.merge_conflicts_json(pretty=True), ledger_path)
    for observation_path in getattr(args, "test_symbol_observations", ()):
        current.add_test_symbol_observations(Path(observation_path).read_bytes())
    return current


def _source_snapshot_project(
    artifact_json: bytes,
    *,
    root: Path,
) -> Project:
    """Load the artifact's hash-matching source bytes from a checkout."""

    artifact = json.loads(artifact_json)
    if (
        not isinstance(artifact, dict)
        or artifact.get("artifact") not in {"map", "query"}
    ):
        raise ValueError("source view requires a canonical Map or Query")
    project_name = artifact.get("project")
    files = artifact.get("files")
    if not isinstance(project_name, str) or not project_name:
        raise ValueError("source artifact has no project identity")
    if not isinstance(files, list):
        raise ValueError("source artifact has no file inventory")
    repository = root.resolve()
    sources: list[Source] = []
    seen: set[str] = set()
    for row in files:
        if not isinstance(row, dict):
            raise ValueError("source selection contains a malformed file row")
        path = row.get("path")
        if not isinstance(path, str) or path in seen:
            raise ValueError("source selection contains an invalid or duplicate path")
        expected = row.get("sha256")
        if not isinstance(expected, str) or len(expected) != 64:
            raise ValueError(f"source artifact has an invalid digest for {path!r}")
        candidate = repository.joinpath(*path.split("/"))
        cursor = repository
        for part in path.split("/"):
            cursor = cursor / part
            if cursor.is_symlink():
                raise ValueError(f"source path traverses a symlink: {path}")
        resolved = candidate.resolve(strict=True)
        try:
            resolved.relative_to(repository)
        except ValueError as error:
            raise ValueError(f"source path escapes repository root: {path}") from error
        if not resolved.is_file():
            raise ValueError(f"source path is not a regular file: {path}")
        data = resolved.read_bytes()
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise ValueError(
                f"source bytes changed since artifact creation: {path} "
                f"(artifact {expected}, live {actual})"
            )
        sources.append(
            Source(
                path=path,
                data=data,
                language=str(row.get("language") or ""),
                layer=str(row.get("layer") or ""),
            )
        )
        seen.add(path)
    return Project(project_name, sources)


def _resolution_from_args(args: argparse.Namespace, *, pretty: bool) -> bytes:
    repository, config_json, _ = _repository_inputs(args)
    return resolve_discovery(
        repository,
        config=config_json or None,
        project=args.project,
        source=args.source,
        only=args.only,
        exclude=args.exclude,
        ignore=not args.no_ignore,
        ignore_files=args.ignore_file,
        default_excludes=not args.no_default_excludes,
        max_file_bytes=args.max_file_bytes,
        max_index_bytes=args.max_index_bytes,
        pretty=pretty,
    )


def _config_main(argv: Sequence[str]) -> int:
    args = config_parser().parse_args(argv)
    try:
        resolution_json = _resolution_from_args(args, pretty=args.pretty)
        resolution = json.loads(resolution_json)
        if args.command == "show":
            _write(resolution_json, args.output)
        else:
            destination = Path(args.output)
            if args.output == "-":
                encoded = (
                    json.dumps(
                        resolution["effective_config"],
                        ensure_ascii=True,
                        indent=2,
                        sort_keys=True,
                    ).encode("utf-8")
                    + b"\n"
                )
                sys.stdout.buffer.write(encoded)
            else:
                if destination.exists() and not args.force:
                    raise ConfigError(
                        f"refusing to replace existing configuration: {destination}"
                    )
                destination.write_text(
                    json.dumps(
                        resolution["effective_config"],
                        ensure_ascii=True,
                        indent=2,
                        sort_keys=True,
                    )
                    + "\n",
                    encoding="utf-8",
                )
        if args.check and _has_error_diagnostics(resolution):
            return 1
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _query_main(
    argv: Sequence[str], *, command: str, default_direction: str
) -> int:
    args = query_parser(command, default_direction=default_direction).parse_args(argv)
    _normalize_query_positional(args)
    progress = _Progress(args.progress)
    try:
        if args.dump:
            if args.view not in {"focused", "source"}:
                raise ValueError("--dump conflicts with a non-source --view")
            if args.compact or args.detail != "standard":
                raise ValueError("--dump conflicts with compact/detail options")
            args.view = "source"
            args.full = True
        if args.resolution and not args.map:
            raise ValueError("--resolution requires --map")
        if args.map and (
            getattr(args, "root_path", None)
            or (args.root_override and args.view != "source")
            or args.no_config
            or _has_discovery_overrides(args)
        ):
            raise ValueError("--map cannot be combined with repository discovery options")
        if args.map and (args.jobs or args.python_provider_timeout is not None):
            raise ValueError(
                "--jobs and --python-provider-timeout apply only to a live repository"
            )
        if args.map and args.test_symbol_observations:
            raise ValueError(
                "--test-symbol-observations requires a live repository, not --map"
            )
        if args.map and args.git_diff:
            raise ValueError("--git-diff requires a live repository, not --map")
        if args.verification_result and (
            args.format != "markdown" or args.view != "changes"
        ):
            raise ValueError(
                "--verification-result requires --format markdown --view changes"
            )
        if args.max_chars < 0:
            raise ValueError("--max-chars must be nonnegative")
        if args.max_seed_distance is not None and args.max_seed_distance < 0:
            raise ValueError("--max-seed-distance must be nonnegative")
        if args.search_limit is not None and not 1 <= args.search_limit <= 100:
            raise ValueError("--search-limit must be from 1 to 100")
        if args.format == "json" and args.max_chars:
            raise ValueError("--max-chars applies only to Markdown")
        if args.format == "json" and (
            args.compact
            or args.full
            or args.dump
            or args.detail != "standard"
            or args.view != "focused"
        ):
            raise ValueError("--view and detail options apply only to Markdown")
        if args.format == "markdown" and args.pretty:
            raise ValueError("--pretty applies only to JSON")
        if args.compact and args.full:
            raise ValueError("--compact and --full conflict")
        if (args.compact or args.full) and args.detail != "standard":
            raise ValueError("--detail conflicts with --compact/--full")
        if args.view == "source" and args.full and args.max_chars:
            raise ValueError("full source view cannot be combined with --max-chars")
        change_set = None
        config_json = b""
        source_project: Optional[Project] = None
        if args.map:
            map_json = Path(args.map).read_bytes()
            resolution_json = (
                Path(args.resolution).read_bytes() if args.resolution else b""
            )
            if args.config:
                config_json = (
                    sys.stdin.buffer.read()
                    if args.config == "-"
                    else Path(args.config).read_bytes()
                )
                _validate_project_configuration(config_json)
            elif args.query_id:
                candidate = Path.cwd() / "archbird.json"
                if not candidate.is_file():
                    raise ConfigError(
                        "named query requires archbird.json or --config with --map"
                    )
                config_json = candidate.read_bytes()
                _validate_project_configuration(config_json)
        else:
            repository, config_json, _ = _repository_inputs(args)
            if args.git_diff:
                change_set = _git_change_set(repository, args.git_diff)
            current = _project_from_args(
                args,
                progress,
                resolved_repository=repository,
                resolved_config_json=config_json,
            )
            source_project = current
            progress.emit({"phase": "rendering", "artifact": "canonical Map"})
            map_json = current.map_json()
            resolution_json = current.resolution_json or b""
            _warn_map_cache_stats(current.map_cache_stats)
        map_document = json.loads(map_json)
        if args.check and _has_error_diagnostics(map_document):
            return 1
        if args.check and args.map:
            producer = map_document.get("tool", {}).get("implementation_sha256")
            if producer != _native.IMPLEMENTATION_SHA256:
                label = producer if isinstance(producer, str) else "missing"
                print(
                    "archbird: check failed: saved Map core "
                    f"{label} does not match active core "
                    f"{_native.IMPLEMENTATION_SHA256}",
                    file=sys.stderr,
                )
                return 1
        ad_hoc_options = {
            "focus": args.focus,
            "paths": args.path,
            "symbols": args.symbol,
            "components": args.component,
            "packages": args.package,
            "artifacts": args.artifact,
            "search": args.search,
            "search_limit": args.search_limit if args.search_limit is not None else 8,
            "change_set": change_set,
            "direction": args.direction or default_direction,
            "producer_policy": (
                "current" if args.check and args.map else "compatible"
            ),
            "depth": args.depth if args.depth is not None else 1,
            "test_depth": args.test_depth if args.test_depth is not None else 8,
        }
        allowed_context_kinds = {
            "files",
            "symbol_calls",
            "symbol_references",
            "test_matches",
        }

        def context_counts(values: Sequence[str], option: str) -> dict[str, int]:
            result: dict[str, int] = {}
            for value in values:
                kind, separator, raw_count = value.partition("=")
                if (
                    separator != "="
                    or kind not in allowed_context_kinds
                    or not raw_count.isdigit()
                ):
                    raise ValueError(
                        f"{option} expects KIND=N for "
                        "files, symbol_calls, symbol_references, or test_matches"
                    )
                if kind in result:
                    raise ValueError(f"{option} repeats {kind}")
                result[kind] = int(raw_count)
            return result

        context: dict[str, object] = {}
        if args.context_profile:
            context["profile"] = args.context_profile
        if args.route_provenance:
            context["provenance"] = args.route_provenance
        if args.route_confidence:
            context["confidence"] = args.route_confidence
        if args.max_seed_distance is not None:
            context["max_seed_distance"] = args.max_seed_distance
        if args.candidate:
            context["candidate"] = args.candidate
        if args.conservative:
            context["conservative"] = args.conservative
        quotas = context_counts(args.context_quota, "--context-quota")
        offsets = context_counts(args.context_offset, "--context-offset")
        if quotas:
            context["quotas"] = quotas
        if offsets:
            context["offsets"] = offsets
        if context:
            ad_hoc_options["context"] = context
        plan = None
        if args.query_id:
            if not config_json:
                raise ConfigError(
                    f"named query {args.query_id!r} requires archbird.json"
                )
            overrides: dict[str, object] = {}
            for name, value in (
                ("focus", args.focus),
                ("paths", args.path),
                ("symbols", args.symbol),
                ("components", args.component),
                ("packages", args.package),
                ("artifacts", args.artifact),
                ("search", args.search),
            ):
                if value:
                    overrides[name] = value
            for name, value in (
                ("search_limit", args.search_limit),
                ("direction", args.direction),
                ("depth", args.depth),
                ("test_depth", args.test_depth),
            ):
                if value is not None:
                    overrides[name] = value
            if context:
                overrides["context"] = context
            plan = compile_named_query(
                config_json,
                args.query_id,
                overrides=overrides,
            )
            query_options = {
                "change_set": change_set,
                "producer_policy": ad_hoc_options["producer_policy"],
                "plan": plan,
            }
        else:
            query_options = ad_hoc_options
        if args.format == "json":
            encoded = query_map_json(
                map_json,
                resolution_json=resolution_json,
                pretty=args.pretty,
                **query_options,
            )
        elif args.view == "source":
            query_json = query_map_json(
                map_json,
                resolution_json=resolution_json,
                **query_options,
            )
            if source_project is None:
                source_project = _source_snapshot_project(
                    query_json,
                    root=Path(args.root_override or "."),
                )
            encoded = render_source_markdown(
                source_project,
                query_json,
                detail=args.detail,
                compact=args.compact,
                full=args.full,
                max_chars=args.max_chars,
            )
        else:
            encoded = query_map_markdown(
                map_json,
                view=args.view,
                detail=args.detail,
                compact=args.compact,
                full=args.full,
                max_chars=args.max_chars,
                verification_result=(
                    Path(args.verification_result).read_bytes()
                    if args.verification_result
                    else b""
                ),
                resolution_json=resolution_json,
                **query_options,
            )
        progress.finish()
        _write(encoded, args.output)
        return 0
    except _native.Error as error:
        progress.clear()
        if getattr(error, "status", None) == 10:
            print(f"archbird: check failed: {error}", file=sys.stderr)
            return 1
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        progress.clear()
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _path_main(argv: Sequence[str]) -> int:
    args = path_parser().parse_args(argv)
    progress = _Progress(args.progress)
    try:
        if args.resolution and not args.map:
            raise ValueError("--resolution requires --map")
        if args.map and (
            args.config
            or args.root_override
            or args.no_config
            or _has_discovery_overrides(args)
        ):
            raise ValueError(
                "--map cannot be combined with repository discovery options"
            )
        if args.map and (args.jobs or args.python_provider_timeout is not None):
            raise ValueError(
                "--jobs and --python-provider-timeout apply only to a live repository"
            )
        if args.map and args.test_symbol_observations:
            raise ValueError(
                "--test-symbol-observations requires a live repository, not --map"
            )
        if not 0 <= args.max_depth <= 64:
            raise ValueError("--max-depth must be from 0 to 64")
        if not 1 <= args.max_paths <= 100:
            raise ValueError("--max-paths must be from 1 to 100")
        if args.max_chars < 0:
            raise ValueError("--max-chars must be nonnegative")
        if args.format == "json" and args.max_chars:
            raise ValueError("--max-chars applies only to Markdown")
        if args.format == "markdown" and args.pretty:
            raise ValueError("--pretty applies only to JSON")
        if args.map:
            map_json = Path(args.map).read_bytes()
            resolution_json = (
                Path(args.resolution).read_bytes() if args.resolution else b""
            )
        else:
            repository, config_json, _ = _repository_inputs(args)
            current = _project_from_args(
                args,
                progress,
                resolved_repository=repository,
                resolved_config_json=config_json,
            )
            progress.emit({"phase": "rendering", "artifact": "canonical Map"})
            map_json = current.map_json()
            resolution_json = current.resolution_json or b""
            _warn_map_cache_stats(current.map_cache_stats)
        map_document = json.loads(map_json)
        if args.check and _has_error_diagnostics(map_document):
            return 1
        endpoint_kind = args.level
        options = {
            "level": args.level,
            "relations": args.relation or None,
            "direction": args.direction,
            "max_depth": args.max_depth,
            "max_paths": args.max_paths,
            "producer_policy": (
                "current" if args.check and args.map else "compatible"
            ),
            "resolution_json": resolution_json,
        }
        source = {
            "kind": args.source_kind or endpoint_kind,
            "patterns": [args.source_pattern],
        }
        target = {
            "kind": args.target_kind or endpoint_kind,
            "patterns": [args.target_pattern],
        }
        canonical = path_map_json(map_json, source, target, **options)
        artifact = json.loads(canonical)
        if args.check:
            if artifact.get("outcome") != "found":
                progress.clear()
                print(
                    "archbird: check failed: connection path outcome is "
                    f"{artifact.get('outcome', 'invalid')}",
                    file=sys.stderr,
                )
                return 1
        if args.format == "json":
            encoded = (
                _native.json_canonicalize(canonical, pretty=True)
                if args.pretty
                else canonical
            )
        else:
            encoded = render_path_markdown(
                canonical,
                max_chars=args.max_chars,
            )
        progress.finish()
        _write(encoded, args.output)
        return 0
    except _native.Error as error:
        progress.clear()
        if getattr(error, "status", None) == 10:
            print(f"archbird: check failed: {error}", file=sys.stderr)
            return 1
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        progress.clear()
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _freshness_main(argv: Sequence[str]) -> int:
    args = freshness_parser().parse_args(argv)
    progress = _Progress(args.progress)
    try:
        snapshot_json = Path(args.snapshot).read_bytes()
        current = _project_from_args(args, progress)
        progress.emit({"phase": "rendering", "artifact": "canonical Map"})
        current_map_json = current.map_json()
        _warn_map_cache_stats(current.map_cache_stats)
        progress.emit({"phase": "rendering", "artifact": "freshness audit"})
        encoded = audit_map_freshness(
            snapshot_json, current_map_json, pretty=args.pretty
        )
        document = json.loads(encoded)
        _write(encoded, args.output)
        progress.finish()
        if not args.check:
            return 0
        return int(
            document.get("status") != "current"
            or _has_error_diagnostics(json.loads(current_map_json))
        )
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


_DIFF_RISK_POLICIES = {
    "public-api": (
        ("public_symbols", "removed_changed"),
        ("package_exports", "removed_changed"),
        ("package_export_origins", "removed_changed"),
        ("package_entrypoint_surfaces", "removed_changed"),
        ("entrypoints", "removed_changed"),
    ),
    "bridges": (("bridges", "any"), ("bridge_surfaces", "any")),
    "calls": (
        ("call_resolutions", "any"),
        ("symbol_calls", "any"),
        ("symbol_references", "any"),
    ),
    "parity": (("parity_gaps", "added_changed"),),
    "tests": (
        ("test_route_evidence", "any"),
        ("test_routes", "removed_changed"),
    ),
    "architecture": (
        ("artifacts", "any"),
        ("build_routes", "any"),
        ("component_routes", "any"),
        ("package_dependencies", "any"),
    ),
}


def _diff_has_risk(document: object, raw_categories: str) -> bool:
    if not isinstance(document, dict) or not isinstance(
        document.get("sections"), dict
    ):
        raise ValueError("native diff result has no sections")
    categories = tuple(
        sorted({part.strip() for part in raw_categories.split(",") if part.strip()})
    )
    unknown = sorted(set(categories) - {*_DIFF_RISK_POLICIES, "all"})
    if unknown:
        raise ValueError(f"diff.check: unknown categories: {', '.join(unknown)}")

    def matches(section: object, policy: str) -> bool:
        if not isinstance(section, dict):
            raise ValueError("native diff section is invalid")
        added = bool(section.get("added"))
        changed = bool(section.get("changed"))
        removed = bool(section.get("removed"))
        if policy == "any":
            return added or changed or removed
        if policy == "removed_changed":
            return removed or changed
        if policy == "added_changed":
            return added or changed
        raise ValueError(f"unknown diff risk policy {policy!r}")

    sections = document["sections"]
    if "all" in categories:
        return any(matches(section, "any") for section in sections.values())
    return any(
        matches(sections[name], policy)
        for category in categories
        for name, policy in _DIFF_RISK_POLICIES[category]
    )


def _diff_main(argv: Sequence[str]) -> int:
    args = diff_parser().parse_args(argv)
    try:
        encoded = diff_maps_json(
            Path(args.before).read_bytes(),
            Path(args.after).read_bytes(),
            pretty=args.pretty,
        )
        document = json.loads(encoded)
        _write(encoded, args.output)
        return int(
            args.check is not None and _diff_has_risk(document, args.check)
        )
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _observe_main(argv: Sequence[str]) -> int:
    args = observe_parser().parse_args(argv)
    try:
        repository = Path(args.root_path or ".").resolve()
        request_path = Path(args.request).resolve()
        encoded = compile_test_observations(
            Path(args.map).read_bytes(),
            request_path.read_bytes(),
            request_directory=request_path.parent,
            repository=repository,
        )
        _write(encoded, args.output)
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _workspace_main(argv: Sequence[str]) -> int:
    args = workspace_parser().parse_args(argv)
    try:
        encoded = Workspace.from_config(
            args.config,
            jobs=args.jobs,
            python_provider_timeout=_python_provider_timeout(args),
            cache_dir=_cache_dir(args),
            cache_max_bytes=_cache_max_bytes(args),
        ).json(pretty=args.pretty)
        document = json.loads(encoded)
        _write(encoded, args.output)
        if not args.check:
            return 0
        if _has_error_diagnostics(document):
            return 1
        return int(
            any(
                isinstance(row, dict)
                and isinstance(row.get("diagnostics"), dict)
                and row["diagnostics"].get("errors", 0)
                for row in document.get("projects", [])
            )
        )
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _constraint_auxiliary_inputs(
    args: argparse.Namespace,
    config_json: bytes,
) -> tuple[
    object,
    Mapping[str, Mapping[str, object]],
    Mapping[str, Mapping[str, object]],
    Optional[str],
]:
    baseline = (
        json.loads(Path(args.baseline).read_bytes()) if args.baseline else None
    )
    observations = _named_json_documents(
        args.observation, option="--observation"
    )
    map_documents = _named_json_documents(args.map_input, option="--map-input")
    resolution_documents = _named_json_documents(
        args.resolution_input, option="--resolution-input"
    )
    unknown_resolutions = set(resolution_documents) - set(map_documents)
    if unknown_resolutions:
        raise ConfigError(
            "--resolution-input has no matching --map-input: "
            + ", ".join(sorted(unknown_resolutions))
        )
    maps = {
        name: {
            "map": document,
            **(
                {"resolution": resolution_documents[name]}
                if name in resolution_documents
                else {}
            ),
        }
        for name, document in map_documents.items()
    }
    config_document = json.loads(config_json)
    constraints = config_document.get("constraints", {})
    constraint_rows = (
        constraints.values()
        if isinstance(constraints, dict)
        else constraints
        if isinstance(constraints, list)
        else ()
    )
    has_expiring_waiver = any(
        isinstance(constraint, dict)
        and any(
            isinstance(waiver, dict) and "expires_on" in waiver
            for waiver in constraint.get("waivers", [])
        )
        for constraint in constraint_rows
    )
    policy_date = args.policy_date
    if policy_date is None and has_expiring_waiver:
        policy_date = datetime.now(timezone.utc).date().isoformat()
    return baseline, maps, observations, policy_date


def _constraint_evaluation_inputs(
    args: argparse.Namespace,
    progress: _Progress,
    *,
    resolved_inputs: Optional[tuple[Path, bytes, Optional[Path]]] = None,
) -> tuple[
    Path,
    bytes,
    bytes,
    bytes,
    Optional[Project],
    object,
    Mapping[str, Mapping[str, object]],
    Mapping[str, Mapping[str, object]],
    Optional[str],
]:
    if args.no_config:
        raise ConfigError(
            "Verify and Plan require constraints from archbird.json; "
            "--no-config is not supported"
        )
    repository, config_json, _ = (
        resolved_inputs
        if resolved_inputs is not None
        else _repository_inputs(args)
    )
    if not config_json:
        raise ConfigError(
            f"no archbird.json found in {repository}; "
            "Verify and Plan require reviewed constraints"
        )
    baseline, maps, observations, policy_date = _constraint_auxiliary_inputs(
        args, config_json
    )
    if args.resolution and not args.map:
        raise ValueError("--resolution requires --map")
    if args.map:
        if args.jobs or args.python_provider_timeout is not None:
            raise ValueError(
                "--jobs and --python-provider-timeout apply only to a live repository"
            )
        map_json = Path(args.map).read_bytes()
        resolution_json = (
            Path(args.resolution).read_bytes() if args.resolution else b""
        )
        project = None
    else:
        project = _project_from_args(
            args,
            progress,
            resolved_repository=repository,
            resolved_config_json=config_json,
        )
        map_json = project.map_json()
        resolution_json = project.resolution_json or b""
        _warn_map_cache_stats(project.map_cache_stats)
    return (
        repository,
        config_json,
        map_json,
        resolution_json,
        project,
        baseline,
        maps,
        observations,
        policy_date,
    )


def _verify_main(argv: Sequence[str]) -> int:
    args = verification_parser().parse_args(argv)
    progress = _Progress(args.progress)
    try:
        if args.max_findings is not None and args.max_findings < 0:
            raise ValueError("--max-findings must be nonnegative")
        if args.full and args.max_findings is not None:
            raise ValueError("--full and --max-findings conflict")
        if args.format != "markdown" and (
            args.full or args.max_findings is not None
        ):
            raise ValueError("--full and --max-findings apply only to Markdown")
        if args.pretty and args.format != "json":
            raise ValueError("--pretty applies only to JSON")
        max_findings = 200 if args.max_findings is None else args.max_findings
        if args.freeze is not None and (
            not args.freeze_owner or not args.freeze_rationale
        ):
            raise ValueError(
                "--freeze requires --freeze-owner and --freeze-rationale"
            )
        if args.freeze is None and (
            args.freeze_owner is not None or args.freeze_rationale is not None
        ):
            raise ValueError("--freeze-owner/--freeze-rationale require --freeze")
        (
            _repository,
            config_json,
            map_json,
            resolution_json,
            _project,
            baseline,
            maps,
            observations,
            policy_date,
        ) = _constraint_evaluation_inputs(args, progress)
        blocking = False
        if args.format == "json":
            encoded = evaluate_constraints_json(
                config_json,
                map_json,
                resolution_json=resolution_json,
                constraint_ids=args.constraint_ids,
                baseline=baseline,
                maps=maps,
                observations=observations,
                policy_date=policy_date,
                pretty=args.pretty,
            )
            verification = json.loads(encoded)
            blocking = bool(verification["summary"]["blocking"])
        else:
            encoded, blocking = _evaluate_constraints_report_with_blocking(
                config_json,
                map_json,
                resolution_json=resolution_json,
                constraint_ids=args.constraint_ids,
                baseline=baseline,
                maps=maps,
                observations=observations,
                policy_date=policy_date,
                format=args.format,
                max_findings=-1 if args.full else max_findings,
                pretty=args.pretty or args.format == "sarif",
            )
        _write(encoded, args.output)
        if args.freeze is not None:
            _write(
                freeze_constraints_json(
                    config_json,
                    map_json,
                    owner=args.freeze_owner,
                    rationale=args.freeze_rationale,
                    baseline=baseline,
                    maps=maps,
                    observations=observations,
                    policy_date=policy_date,
                    resolution_json=resolution_json,
                    pretty=True,
                ),
                args.freeze,
            )
        progress.finish()
        if not args.check:
            return 0
        return int(blocking)
    except (ConfigError, OSError, RuntimeError, ValueError, _native.Error) as error:
        progress.clear()
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _plan_main(argv: Sequence[str]) -> int:
    args = plan_parser().parse_args(argv)
    progress = _Progress(args.progress)
    try:
        if args.pretty and args.format != "json":
            raise ValueError("--pretty requires --format json")
        if args.constraint_ids and _query_positional_is_root(
            args.constraint_ids[0]
        ):
            positional_root = Path(args.constraint_ids.pop(0)).resolve()
            if (
                args.root_override
                and positional_root != Path(args.root_override).resolve()
            ):
                raise ConfigError(
                    "positional ROOT and --root select different directories"
                )
            args.root_override = str(positional_root)
        repository_hint = Path(args.root_override or ".").resolve()
        if args.before_map and args.git_diff:
            raise ValueError("--before-map and --git-diff are mutually exclusive")
        if args.map and args.git_diff:
            raise ValueError("--git-diff requires a live repository, not --map")
        transient_output = _repository_artifact_path(
            repository_hint, args.output
        )
        args._transient_exclude = (
            (transient_output,) if transient_output is not None else ()
        )
        resolved_inputs = _repository_inputs(args)
        repository, config_json, _ = resolved_inputs
        if not config_json:
            raise ConfigError(
                f"no archbird.json found in {repository}; "
                "Verify and Plan require reviewed constraints"
            )
        before_map_json = (
            Path(args.before_map).read_bytes() if args.before_map else b""
        )
        if args.git_diff:
            progress.emit(
                {
                    "phase": "discovery",
                    "state": "historical",
                    "revision": args.git_diff,
                }
            )
            with _git_snapshot(repository, args.git_diff) as snapshot:
                before_project = _project_from_args(
                    args,
                    progress,
                    resolved_repository=snapshot,
                    resolved_config_json=config_json,
                )
                before_map_json = before_project.map_json()
                del before_project
        (
            repository,
            config_json,
            map_json,
            resolution_json,
            project,
            baseline,
            maps,
            observations,
            policy_date,
        ) = _constraint_evaluation_inputs(
            args,
            progress,
            resolved_inputs=resolved_inputs,
        )
        verification_json = evaluate_constraints_json(
            config_json,
            map_json,
            resolution_json=resolution_json,
            baseline=baseline,
            maps=maps,
            observations=observations,
            policy_date=policy_date,
            pretty=False,
        )
        map_document = json.loads(map_json)
        if _has_error_diagnostics(map_document):
            raise ValueError(
                "Plan requires a Map without error diagnostics; "
                "fix Map evidence before deriving edits"
            )
        if project is None:
            project = _discover_project_from_args(
                args,
                progress,
                resolved_repository=repository,
                resolved_config_json=config_json,
            )
        rename_directives: dict[str, str] = {}
        for directive in args.rename:
            old, separator, new = directive.partition("=")
            if (
                separator != "="
                or not old
                or not new
                or old in rename_directives
            ):
                raise ValueError(
                    "--rename requires unique non-empty OLD=NEW directives"
                )
            rename_directives[old] = new
        redirect_directives: dict[str, str] = {}
        for directive in args.redirect:
            old, separator, new = directive.partition("=")
            if (
                separator != "="
                or not old
                or not new
                or old in redirect_directives
            ):
                raise ValueError(
                    "--redirect requires unique non-empty FROM=TO directives"
                )
            redirect_directives[old] = new
        request: dict[str, object] = {}
        configuration_plan = json.loads(
            _native.project_configuration_compile(config_json)
        )
        if configuration_plan.get("gates"):
            request["gates"] = configuration_plan["gates"]
        if args.constraint_ids:
            request["constraint_ids"] = args.constraint_ids
        if rename_directives:
            request["renames"] = rename_directives
        if redirect_directives:
            request["redirects"] = redirect_directives
        if args.objective:
            request["objective"] = args.objective
        encoded = project.plan_json(
            verification_json,
            map_json=map_json,
            before_map_json=before_map_json,
            request_json=(
                json.dumps(
                    request,
                    allow_nan=False,
                    ensure_ascii=True,
                    separators=(",", ":"),
                    sort_keys=True,
                ).encode("utf-8")
                if request
                else b""
            ),
            pretty=args.pretty,
        )
        plan = json.loads(encoded)
        rendered = (
            render_plan_markdown(encoded)
            if args.format == "markdown"
            else encoded
        )
        _write(rendered, args.output)
        if args.output != "-":
            executable = sum(
                1
                for item in plan["items"]
                if isinstance(item, Mapping) and item.get("executable") is True
            )
            print(
                "Result: "
                f"items={len(plan['items'])}; "
                f"executable={executable}; "
                f"non-executable={len(plan['items']) - executable}; "
                f"unknowns={len(plan['unknowns'])}; "
                f"preserved-constraints={len(plan['preserved_constraints'])}"
            )
        progress.finish()
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError, _native.Error) as error:
        progress.clear()
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _act_project(
    args: argparse.Namespace,
    repository: Path,
    config_json: bytes,
    progress: _Progress,
) -> Project:
    progress.emit({"phase": "discovery", "state": "start"})
    project = Project.from_repository(
        repository,
        config=config_json,
        _transient_exclude=getattr(args, "_transient_exclude", ()),
        scan=False,
        jobs=args.jobs,
    )
    progress.emit({"phase": "selected", "files": len(project.sources)})
    project.scan(
        jobs=args.jobs,
        python_provider_timeout=_python_provider_timeout(args),
        cache_dir=_cache_dir(args),
        cache_max_bytes=_cache_max_bytes(args),
        progress=progress.emit,
    )
    _warn_cache_stats(project.cache_stats)
    _warn_map_cache_stats(project.map_cache_stats)
    return project


def _act_overlay_project(
    args: argparse.Namespace,
    before: Project,
    config_json: bytes,
    overlay: Mapping[str, bytes | None],
    progress: _Progress,
) -> Project:
    progress.emit({"phase": "discovery", "state": "overlay"})
    project = before.with_source_overlay(
        overlay,
        config=config_json,
        scan=False,
    )
    progress.emit({"phase": "selected", "files": len(project.sources)})
    project.scan(
        jobs=args.jobs,
        python_provider_timeout=_python_provider_timeout(args),
        cache_dir=_cache_dir(args),
        cache_max_bytes=_cache_max_bytes(args),
        progress=progress.emit,
    )
    _warn_cache_stats(project.cache_stats)
    _warn_map_cache_stats(project.map_cache_stats)
    return project


def _act_main(argv: Sequence[str]) -> int:
    args = act_parser().parse_args(argv)
    progress = _Progress(args.progress)
    try:
        if args.pretty and args.format != "json":
            raise ValueError("--pretty applies only to JSON")
        plan_path = Path(args.plan)
        plan_metadata = plan_path.lstat()
        if stat.S_ISLNK(plan_metadata.st_mode):
            raise ValueError("Plan input must not be a symbolic link")
        if not stat.S_ISREG(plan_metadata.st_mode):
            raise ValueError("Plan input must be a regular file")
        if plan_metadata.st_size > MAX_PLAN_BYTES:
            raise ValueError(
                f"Plan exceeds the {MAX_PLAN_BYTES}-byte input limit"
            )
        plan_bytes = plan_path.read_bytes()
        if len(plan_bytes) > MAX_PLAN_BYTES:
            raise ValueError(
                f"Plan exceeds the {MAX_PLAN_BYTES}-byte input limit"
            )
        repository = Path(args.root_override or ".").resolve()
        executor_submissions, submission_paths = _act_executor_submissions(
            args.submit
        )
        transient_artifacts = [
            path
            for path in (
                _repository_artifact_path(repository, str(plan_path)),
                _repository_artifact_path(repository, args.output),
                *(
                    _repository_artifact_path(repository, str(path))
                    for path in submission_paths
                ),
            )
            if path is not None
        ]
        args._transient_exclude = tuple(dict.fromkeys(transient_artifacts))
        repository, config_json, config_path = _repository_inputs(args)
        if not config_json:
            raise ConfigError(
                f"no archbird.json found in {repository}; "
                "Act acceptance requires reviewed constraints"
            )
        baseline, maps, observations, policy_date = _constraint_auxiliary_inputs(
            args, config_json
        )
        before_project = _act_project(args, repository, config_json, progress)
        before_map = before_project.map_json()
        before_verification = evaluate_constraints_json(
            config_json,
            before_map,
            resolution_json=before_project.resolution_json or b"",
            baseline=baseline,
            maps=maps,
            observations=observations,
            policy_date=policy_date,
            pretty=False,
        )
        source_metadata = observe_plan_sources(
            repository, plan_bytes, executor_submissions
        )
        materialized_act = _native.act_materialize(
            before_project._capsule,
            plan_bytes,
            before_map,
            before_verification,
            source_metadata,
            executor_submissions,
        )
        overlay = act_overlay(materialized_act)
        after_config_json = config_json
        if config_path is not None:
            try:
                relative_config = config_path.relative_to(repository).as_posix()
            except ValueError:
                relative_config = None
            if relative_config is not None and relative_config in overlay:
                overlaid_config = overlay[relative_config]
                if overlaid_config is None:
                    raise ValueError(
                        "Act cannot remove the project configuration used for "
                        "acceptance"
                    )
                after_config_json = overlaid_config
                _validate_project_configuration(after_config_json)
        after_project = _act_overlay_project(
            args, before_project, after_config_json, overlay, progress
        )
        after_map = after_project.map_json()
        after_verification = evaluate_constraints_json(
            after_config_json,
            after_map,
            resolution_json=after_project.resolution_json or b"",
            baseline=baseline,
            maps=maps,
            observations=observations,
            policy_date=policy_date,
            pretty=False,
        )
        gate_results = run_act_gates(repository, materialized_act)
        try:
            accepted_act = _native.act_accept(
                materialized_act,
                before_map,
                after_map,
                after_verification,
                gate_results,
            )
        except _native.Error as error:
            details = gate_failure_details(gate_results)
            if details:
                raise RuntimeError(f"{error}\n{details}") from error
            raise
        _write(
            render_act(
                repository,
                accepted_act,
                format=args.format,
                pretty=args.pretty,
            ),
            args.output,
        )
        progress.finish()
        return 0
    except (
        ConfigError,
        json.JSONDecodeError,
        OSError,
        RuntimeError,
        ValueError,
        _native.Error,
    ) as error:
        progress.clear()
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _apply_main(argv: Sequence[str]) -> int:
    args = apply_parser().parse_args(argv)
    try:
        act_path = Path(args.act)
        metadata = act_path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError("Act input must not be a symbolic link")
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("Act input must be a regular file")
        if metadata.st_size > MAX_ACT_BYTES:
            raise ValueError(
                f"Act exceeds the {MAX_ACT_BYTES}-byte input limit"
            )
        act_bytes = act_path.read_bytes()
        if len(act_bytes) > MAX_ACT_BYTES:
            raise ValueError(
                f"Act exceeds the {MAX_ACT_BYTES}-byte input limit"
            )
        transitions = apply_accepted_act(
            Path(args.root_override or "."), act_bytes
        )
        state = "applied" if transitions else "already-satisfied"
        print(f"Result: applied-transitions={transitions}; state={state}")
        return 0
    except (json.JSONDecodeError, OSError, ValueError, _native.Error) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _export_main(argv: Sequence[str]) -> int:
    args = export_parser().parse_args(argv)
    try:
        if args.format == "okf":
            if args.output == "-":
                raise ValueError("export okf requires -o/--output directory")
            export_okf_bundle(
                args.map,
                args.output,
                verification_path=args.verification,
                replace=args.replace,
            )
            return 0
        if args.verification or args.replace:
            raise ValueError(
                "--verification and --replace apply only to export okf"
            )
        encoded = export_graph(
            Path(args.map).read_bytes(),
            format=args.format,
            view=args.view,
            direction=args.direction,
            max_nodes=args.max_nodes,
            max_edge_names=args.max_edge_names,
        )
        _write(encoded, args.output)
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _okf_main(argv: Sequence[str]) -> int:
    args = okf_parser().parse_args(argv)
    try:
        source = parse_okf_bundle(args.bundle)
        query = b""
        if args.command == "query":
            query = okf_query_input(
                concepts=args.concept,
                types=args.types,
                tags=args.tag,
                text=args.text,
                requirements=args.requirement,
            )
        encoded = analyze_okf_source(
            source,
            query_json=query,
            format=args.format,
            include_body=args.command == "query",
            pretty=True,
        )
        _write(encoded, args.output)
        should_check = args.command == "validate" or getattr(args, "check", False)
        if not should_check:
            return 0
        result = json.loads(
            analyze_okf_source(source, format="json", pretty=False)
        )
        return int(result["summary"]["errors"] != 0)
    except (ConfigError, OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _serve_main(argv: Sequence[str]) -> int:
    args = serve_parser().parse_args(argv)
    try:
        if args.jobs < 0:
            raise ValueError("--jobs must be zero or positive")
        _python_provider_timeout(args)
        if args.port < 0 or args.port > 65535:
            raise ValueError("--port must be between 0 and 65535")
        repository, config_json, config_path = _repository_inputs(args)
        from .serve import create_live_server

        server = create_live_server(
            app=args.app,
            config=config_path,
            config_json=(
                None if config_path is not None or not config_json else config_json
            ),
            host=args.host,
            no_config=args.no_config,
            port=args.port,
            project_options={
                "default_excludes": not args.no_default_excludes,
                "exclude": tuple(args.exclude),
                "ignore": not args.no_ignore,
                "ignore_files": tuple(args.ignore_file),
                "jobs": args.jobs,
                "python_provider_timeout": _python_provider_timeout(args),
                "cache_dir": (
                    str(_cache_dir(args)) if _cache_dir(args) is not None else None
                ),
                "cache_max_bytes": _cache_max_bytes(args),
                "max_file_bytes": args.max_file_bytes,
                "max_index_bytes": args.max_index_bytes,
                "only": tuple(args.only),
                "project": args.project,
                "source": tuple(args.source),
            },
            root=repository,
        )
        print(server.url, flush=True)
        stopped = threading.Event()
        prior = {}

        def stop(_signum: int, _frame: object) -> None:
            stopped.set()

        for signum in (signal.SIGINT, signal.SIGTERM):
            prior[signum] = signal.signal(signum, stop)
        try:
            while not stopped.wait(3600):
                pass
        finally:
            server.close()
            for signum, handler in prior.items():
                signal.signal(signum, handler)
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _mcp_main(argv: Sequence[str]) -> int:
    args = mcp_parser().parse_args(argv)
    repository_host = None
    try:
        if args.config == "-":
            raise ValueError("archbird mcp reserves stdin for MCP; use a config file")
        if args.jobs < 0:
            raise ValueError("--jobs must be zero or positive")
        _python_provider_timeout(args)
        repository, config_json, config_path = _repository_inputs(args)
        from .mcp import serve_stdio
        from .serve import LiveRepository

        repository_host = LiveRepository(
            repository,
            config=config_path,
            config_json=(
                None if config_path is not None or not config_json else config_json
            ),
            no_config=args.no_config,
            project_options={
                "default_excludes": not args.no_default_excludes,
                "exclude": tuple(args.exclude),
                "ignore": not args.no_ignore,
                "ignore_files": tuple(args.ignore_file),
                "jobs": args.jobs,
                "python_provider_timeout": _python_provider_timeout(args),
                "cache_dir": (
                    str(_cache_dir(args)) if _cache_dir(args) is not None else None
                ),
                "cache_max_bytes": _cache_max_bytes(args),
                "max_file_bytes": args.max_file_bytes,
                "max_index_bytes": args.max_index_bytes,
                "only": tuple(args.only),
                "project": args.project,
                "source": tuple(args.source),
            },
        )
        repository_host.start()
        return serve_stdio(repository_host)
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2
    finally:
        if repository_host is not None:
            repository_host.close()


def main(argv: Optional[Sequence[str]] = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments[:1] in (["-h"], ["--help"], ["help"]):
        print(_top_level_help(), end="")
        return 0
    if arguments[:1] == ["--version"]:
        print(__version__)
        return 0
    if (
        arguments
        and arguments[0] not in _COMMANDS
        and not _map_shortcut(arguments)
    ):
        print(
            f"archbird: error: unknown command {arguments[0]!r}; "
            "run `archbird --help`",
            file=sys.stderr,
        )
        return 2
    if arguments and arguments[0] == "map":
        arguments = arguments[1:]
    if arguments and arguments[0] == "query":
        return _query_main(arguments[1:], command="query", default_direction="both")
    if arguments and arguments[0] == "impact":
        return _query_main(
            arguments[1:], command="impact", default_direction="upstream"
        )
    if arguments and arguments[0] == "path":
        return _path_main(arguments[1:])
    if arguments and arguments[0] == "diff":
        return _diff_main(arguments[1:])
    if arguments and arguments[0] == "observe":
        return _observe_main(arguments[1:])
    if arguments and arguments[0] == "freshness":
        return _freshness_main(arguments[1:])
    if arguments and arguments[0] == "workspace":
        return _workspace_main(arguments[1:])
    if arguments and arguments[0] == "verify":
        return _verify_main(arguments[1:])
    if arguments and arguments[0] == "plan":
        return _plan_main(arguments[1:])
    if arguments and arguments[0] == "act":
        return _act_main(arguments[1:])
    if arguments and arguments[0] == "apply":
        return _apply_main(arguments[1:])
    if arguments and arguments[0] == "export":
        return _export_main(arguments[1:])
    if arguments and arguments[0] == "okf":
        return _okf_main(arguments[1:])
    if arguments and arguments[0] == "serve":
        return _serve_main(arguments[1:])
    if arguments and arguments[0] == "mcp":
        return _mcp_main(arguments[1:])
    if arguments and arguments[0] == "config":
        return _config_main(arguments[1:])
    if arguments and arguments[0] == "support":
        return _support_main(arguments[1:])
    args = parser().parse_args(arguments)
    progress = _Progress(args.progress)
    try:
        if args.dump:
            if args.view not in {"overview", "source"}:
                raise ValueError("--dump conflicts with a non-source --view")
            if args.compact or args.detail != "standard":
                raise ValueError("--dump conflicts with compact/detail options")
            args.view = "source"
            args.full = True
        if args.max_chars < 0:
            raise ValueError("--max-chars must be nonnegative")
        if args.format == "json" and args.max_chars:
            raise ValueError("--max-chars applies only to Markdown")
        if args.format == "json" and (
            args.full
            or args.compact
            or args.dump
            or args.detail != "standard"
            or args.view != "overview"
            or args.group_by
            or args.level
            or args.relations is not None
            or args.overlay is not None
        ):
            raise ValueError(
                "--view, graph projection axes, and detail options apply only to Markdown"
            )
        if args.compact and args.full:
            raise ValueError("--compact and --full conflict")
        if args.compact and args.detail != "standard":
            raise ValueError("--compact conflicts with explicit --detail")
        if args.full and args.detail != "standard":
            raise ValueError("--full conflicts with explicit --detail")
        if args.view == "source" and (
            args.group_by
            or args.level
            or args.relations is not None
            or args.overlay is not None
        ):
            raise ValueError(
                "source view does not accept graph grouping, level, relations, or overlays"
            )
        if args.view == "source" and args.full and args.max_chars:
            raise ValueError("full source view cannot be combined with --max-chars")
        if args.format == "markdown" and args.pretty:
            raise ValueError("--pretty applies only to JSON")
        project = _project_from_args(args, progress)
        progress.emit({"phase": "rendering", "artifact": "canonical Map"})
        if args.format == "json" and not args.check:
            _write_project_map(project, args.output, pretty=args.pretty)
            _warn_map_cache_stats(project.map_cache_stats)
            progress.finish()
            return 0
        map_json = project.map_json(pretty=args.pretty and args.format == "json")
        _warn_map_cache_stats(project.map_cache_stats)
        document = json.loads(map_json)
        encoded = (
            map_json
            if args.format == "json"
            else render_source_markdown(
                project,
                map_json,
                detail=args.detail,
                compact=args.compact,
                full=args.full,
                max_chars=args.max_chars,
            )
            if args.view == "source"
            else render_map_markdown(
                map_json,
                view=args.view,
                detail="compact" if args.compact else args.detail,
                full=args.full,
                max_chars=args.max_chars,
                group_by=args.group_by,
                level=args.level,
                relations=(
                    tuple(
                        part.strip()
                        for value in args.relations
                        for part in value.split(",")
                        if part.strip()
                    )
                    if args.relations is not None
                    else None
                ),
                overlays=(
                    tuple(
                        part.strip()
                        for value in args.overlay
                        for part in value.split(",")
                        if part.strip()
                    )
                    if args.overlay is not None
                    else None
                ),
                resolution_json=project.resolution_json or b"",
            )
        )
        _write(encoded, args.output)
        progress.finish()
        if args.check and any(
            row.get("severity") == "error" for row in document["diagnostics"]
        ):
            return 1
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
