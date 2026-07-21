"""Command line for deterministic repository maps and architecture contracts."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
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
    Project,
    Verification,
    Workspace,
    audit_map_freshness,
    change_contract,
    change_proposal,
    change_verify,
    compile_test_observations,
    compile_verification_recipe,
    draft_verification_suite,
    diff_maps_json,
    export_graph,
    export_okf_bundle,
    analyze_okf_source,
    query_map_markdown,
    query_map_json,
    render_map_markdown,
    resolve_discovery,
    verification_plan_json,
    verification_recipe_catalog,
)
from .adapters.okf.parser import okf_query_input, parse_okf_bundle


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
        prog="archbird",
        description="Generate a deterministic architecture map.",
    )
    result.add_argument("root_path", nargs="?", help="repository root (default: .)")
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument(
        "--no-config", action="store_true", help="ignore root project configuration"
    )
    result.add_argument("--root", dest="root_override", help="compatibility root override")
    _add_discovery_options(result)
    result.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Python analyzer processes; 0 selects automatically",
    )
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
        choices=("overview", "architecture", "audit"),
        default="overview",
        help="human Markdown projection (default: overview)",
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
        "--max-chars",
        type=int,
        default=0,
        help="maximum Markdown characters; omit only whole sections and ranked file blocks",
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
    result.add_argument("root_path", nargs="?", help="repository root (default: .)")
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument("--map", help="saved canonical Map JSON")
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--root", dest="root_override", help="compatibility root override")
    _add_discovery_options(result)
    result.add_argument("--jobs", type=int, default=0)
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
        metavar="TEXT",
        help=(
            "rank deterministic candidate seeds from names, paths, signatures, "
            "component descriptions, and package/artifact metadata; repeatable"
        ),
    )
    result.add_argument(
        "--search-limit",
        type=int,
        default=8,
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
            "overlay exact-path-relevant checks and findings from one canonical "
            "verification result onto a Markdown changes view"
        ),
    )
    result.add_argument("--depth", type=int, default=1)
    result.add_argument("--test-depth", type=int, default=8)
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
        default=default_direction,
    )
    result.add_argument(
        "--format", choices=("markdown", "json"), default="markdown"
    )
    result.add_argument(
        "--view",
        choices=("focused", "changes"),
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
    result.add_argument("--jobs", type=int, default=0)
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
    result.add_argument("root_path", nargs="?", help="repository root (default: .)")
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="live project configuration JSON")
    source.add_argument("--no-config", action="store_true")
    result.add_argument("--root", dest="root_override")
    _add_discovery_options(result)
    result.add_argument("--jobs", type=int, default=0)
    _add_cache_options(result)
    result.add_argument("--host", default="127.0.0.1", choices=("127.0.0.1", "::1"))
    result.add_argument("--port", type=int, default=4177)
    result.add_argument("--app", help=argparse.SUPPRESS)
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
    result.add_argument("--jobs", type=int, default=0)
    _add_cache_options(result)
    result.add_argument("--check", action="store_true")
    result.add_argument("-o", "--output", default="-")
    return result


def verification_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird verify",
        description=(
            "Evaluate a reviewed architecture suite, discovering a conventional "
            "suite from ROOT when --config is omitted."
        ),
    )
    result.add_argument(
        "root_path",
        nargs="?",
        metavar="ROOT",
        help="repository containing a conventional verification suite (default: .)",
    )
    source = result.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="verification suite JSON")
    source.add_argument(
        "--init",
        metavar="PROJECT_CONFIG",
        help="draft a candidate component-edge suite for human review",
    )
    result.add_argument(
        "--init-root",
        help="local project root override used only while drafting --init",
    )
    result.add_argument(
        "--project-root",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="local project root override; never enters canonical evidence",
    )
    result.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Python analyzer processes; 0 selects automatically",
    )
    _add_cache_options(result)
    result.add_argument(
        "--baseline",
        help="classify findings against an explicit frozen baseline",
    )
    result.add_argument(
        "--freeze",
        help="write/update a reviewed violation and coverage-ratchet baseline",
    )
    result.add_argument("--freeze-owner", help="owner recorded by --freeze")
    result.add_argument(
        "--freeze-rationale", help="review rationale recorded by --freeze"
    )
    result.add_argument("-o", "--output", default="-", help="JSON output or -")
    result.add_argument(
        "--format",
        choices=("json", "markdown", "sarif", "junit"),
        default="json",
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
        default=200,
        help="maximum findings in compact Markdown",
    )
    result.add_argument("--pretty", action="store_true", help="pretty JSON")
    result.add_argument(
        "--check",
        action="store_true",
        help="exit nonzero when verification contains blocking evidence",
    )
    result.add_argument("--version", action="version", version=__version__)
    return result


_BYTE_SIZE_UNITS = {
    "": 1,
    "b": 1,
    "k": 1024,
    "kib": 1024,
    "m": 1024**2,
    "mib": 1024**2,
    "g": 1024**3,
    "gib": 1024**3,
    "t": 1024**4,
    "tib": 1024**4,
    "kb": 1000,
    "mb": 1000**2,
    "gb": 1000**3,
    "tb": 1000**4,
}


def _byte_size(value: str) -> int:
    match = re.fullmatch(r"([0-9]+)([A-Za-z]*)", value)
    if match is None or match.group(2).lower() not in _BYTE_SIZE_UNITS:
        raise argparse.ArgumentTypeError(
            "expected an integer byte size such as 1048576, 1MiB, or 1MB"
        )
    number = int(match.group(1)) * _BYTE_SIZE_UNITS[match.group(2).lower()]
    if number > (1 << 64) - 1:
        raise argparse.ArgumentTypeError("byte size exceeds the unsigned 64-bit limit")
    return number


def _add_recipe_check_options(
    result: argparse.ArgumentParser, *, reviewed: bool
) -> None:
    result.add_argument("--max", required=True, type=_byte_size)
    result.add_argument(
        "--include",
        action="append",
        default=[],
        metavar="GLOB",
        help="select mapped repository-relative paths; repeatable",
    )
    result.add_argument(
        "--exclude",
        action="append",
        default=[],
        metavar="GLOB",
        help="omit paths from recipe selection; repeatable",
    )
    result.add_argument(
        "--allow-empty",
        action="store_true",
        help="permit an empty selection as explicit policy",
    )
    result.add_argument("--id", default="MAX-FILE-BYTES")
    result.add_argument(
        "--owner", required=reviewed, default=None if reviewed else "command-line"
    )
    result.add_argument(
        "--rationale",
        required=reviewed,
        default=None if reviewed else "Explicit one-shot max-file-bytes policy.",
    )
    result.add_argument(
        "--severity", choices=("error", "warning", "info"), default="error"
    )
    result.add_argument("--requirement")
    result.add_argument("--tag", action="append", default=[])


def verification_recipe_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird verify recipe",
        description=(
            "Inspect or run portable verification recipes backed by the normal "
            "Verify evidence and check kernel."
        ),
    )
    commands = result.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list", help="list available recipes")
    listing.add_argument("--pretty", action="store_true")
    listing.add_argument("-o", "--output", default="-")
    showing = commands.add_parser("show", help="show one recipe contract")
    showing.add_argument("recipe")
    showing.add_argument("--pretty", action="store_true")
    showing.add_argument("-o", "--output", default="-")

    compiling = commands.add_parser(
        "compile", help="compile explicit recipe policy into a reviewed suite"
    )
    compiling.add_argument("recipe", choices=("max-file-bytes",))
    _add_recipe_check_options(compiling, reviewed=True)
    compiling.add_argument(
        "--map-path",
        default="ARCHBIRD.json",
        help="portable Map path recorded in the generated suite",
    )
    compiling.add_argument("--pretty", action="store_true")
    compiling.add_argument("--force", action="store_true")
    compiling.add_argument("-o", "--output", default="archbird.verify.json")

    running = commands.add_parser(
        "run", help="run explicit recipe policy against a fresh repository Map"
    )
    running.add_argument("recipe", choices=("max-file-bytes",))
    running.add_argument("root_path", nargs="?", metavar="ROOT")
    source = running.add_mutually_exclusive_group()
    source.add_argument("-c", "--config", help="project configuration JSON")
    source.add_argument("--no-config", action="store_true")
    running.add_argument("--root", dest="root_override")
    running.add_argument("--project")
    running.add_argument("--jobs", type=int, default=0)
    _add_cache_options(running)
    _add_progress_options(running)
    _add_recipe_check_options(running, reviewed=False)
    running.add_argument(
        "--format",
        choices=("json", "markdown", "sarif", "junit"),
        default="markdown",
    )
    running.add_argument("--full", action="store_true")
    running.add_argument("--max-findings", type=int, default=200)
    running.add_argument("--pretty", action="store_true")
    running.add_argument("--check", action="store_true")
    running.add_argument("-o", "--output", default="-")
    return result


def plan_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird plan",
        description="Compile one derived proposal from one verification finding.",
    )
    result.add_argument("--verification", required=True)
    result.add_argument("--finding", required=True)
    result.add_argument("--format", choices=("json", "markdown"), default="json")
    result.add_argument("--full", action="store_true")
    result.add_argument("--max-candidates", type=int, default=20)
    result.add_argument("--pretty", action="store_true")
    result.add_argument("-o", "--output", default="-")
    return result


def contract_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird contract",
        description="Seal explicit review metadata as an asserted change contract.",
    )
    result.add_argument("--proposal", required=True)
    result.add_argument("--objective", required=True)
    result.add_argument("--owner", required=True)
    result.add_argument("--rationale", required=True)
    preserved = result.add_mutually_exclusive_group()
    preserved.add_argument("--preserve-check", action="append", default=[])
    preserved.add_argument("--preserve-all", action="store_true")
    result.add_argument("--select-candidate", action="append", default=[])
    result.add_argument("--format", choices=("json", "markdown"), default="json")
    result.add_argument("--pretty", action="store_true")
    result.add_argument("-o", "--output", default="-")
    return result


def verify_plan_parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="archbird verify-plan",
        description="Judge a reviewed fact transition without executing projects.",
    )
    result.add_argument("--proposal", required=True)
    result.add_argument("--contract", required=True)
    result.add_argument("--before-verification", required=True)
    result.add_argument("--after-verification", required=True)
    result.add_argument(
        "--format",
        choices=("json", "markdown", "sarif", "junit"),
        default="json",
    )
    result.add_argument("--pretty", action="store_true")
    result.add_argument("--check", action="store_true")
    result.add_argument("-o", "--output", default="-")
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
    result.add_argument("--proposal", help="canonical change-proposal JSON")
    result.add_argument("--contract", help="canonical change-contract JSON")
    result.add_argument("--result", help="canonical change-result JSON")
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
        description="Validate and query OKF metadata without treating prose as checks.",
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


def _project_roots(values: Sequence[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if not separator or not name or not raw_path:
            raise ConfigError(
                f"--project-root: expected NAME=PATH, got {value!r}"
            )
        if name in result:
            raise ConfigError(f"--project-root: duplicate project {name!r}")
        result[name] = Path(raw_path).resolve()
    return result


_VERIFICATION_SUITE_NAMES = (
    "archbird.verify.json",
    ".archbird.verify.json",
    "architecture.verify.json",
)


def _discover_verification_suite(root_path: Optional[str]) -> Path:
    root = Path(root_path or ".").resolve()
    if not root.is_dir():
        raise ConfigError(f"verification root is not a directory: {root}")
    candidates: list[Path] = []
    for name in _VERIFICATION_SUITE_NAMES:
        candidate = root / name
        try:
            metadata = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISREG(metadata.st_mode):
            candidates.append(candidate)
    if len(candidates) > 1:
        names = ", ".join(path.name for path in candidates)
        raise ConfigError(f"verification root contains multiple suites: {names}")
    if not candidates:
        expected = ", ".join(_VERIFICATION_SUITE_NAMES)
        raise ConfigError(
            f"no verification suite found in {root}; expected one of: {expected}. "
            "Map can discover implementation evidence, but Verify requires "
            "reviewed architecture intent"
        )
    return candidates[0]


def _has_error_diagnostics(document: object) -> bool:
    return isinstance(document, dict) and any(
        isinstance(row, dict) and row.get("severity") == "error"
        for row in document.get("diagnostics", [])
    )


def _config_root(config_json: bytes) -> str:
    try:
        document = json.loads(config_json)
    except (UnicodeDecodeError, ValueError) as error:
        raise ConfigError(f"invalid project configuration JSON: {error}") from error
    if not isinstance(document, dict):
        raise ConfigError("project configuration must be an object")
    value = document.get("root", ".")
    if not isinstance(value, str) or not value:
        raise ConfigError("project configuration root must be a nonempty string")
    return value


def _repository_inputs(args: argparse.Namespace) -> tuple[Path, bytes, Optional[Path]]:
    positional = Path(args.root_path).resolve() if args.root_path else None
    override = Path(args.root_override).resolve() if args.root_override else None
    if positional is not None and override is not None and positional != override:
        raise ConfigError("positional ROOT and --root select different directories")
    selected = positional or override or Path.cwd().resolve()
    config_path: Optional[Path] = None
    config_json = b""
    if args.config:
        if args.config == "-":
            config_json = sys.stdin.buffer.read()
            if positional is None and override is None:
                selected = (selected / _config_root(config_json)).resolve()
        else:
            config_path = Path(args.config).resolve()
            try:
                config_json = config_path.read_bytes()
            except OSError as error:
                raise ConfigError(
                    f"cannot read configuration: {config_path}: {error}"
                ) from error
        if positional is None and override is None and config_path is not None:
            selected = (config_path.parent / _config_root(config_json)).resolve()
    elif not args.no_config:
        candidates = []
        for candidate in (selected / "archbird.json", selected / ".archbird.json"):
            try:
                metadata = candidate.lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISREG(metadata.st_mode):
                candidates.append(candidate)
        if len(candidates) > 1:
            raise ConfigError(
                "repository contains both archbird.json and .archbird.json"
            )
        if candidates:
            config_path = candidates[0].resolve()
            config_json = config_path.read_bytes()
            selected = (selected / _config_root(config_json)).resolve()
    if not selected.is_dir():
        raise ConfigError(f"root is not a directory: {selected}")
    return selected, config_json, config_path


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


def _project_from_args(
    args: argparse.Namespace, progress: Optional[_Progress] = None
) -> Project:
    if progress is not None:
        progress.emit({"phase": "discovery", "state": "start"})
    repository, config_json, _ = _repository_inputs(args)
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
        scan=False,
        jobs=args.jobs,
    )
    if progress is not None:
        progress.emit({"phase": "selected", "files": len(current.sources)})
    ledger_path = getattr(args, "merge_ledger", None)
    if ledger_path and ledger_path == getattr(args, "output", None):
        raise ValueError("--merge-ledger and --output must be different paths")
    try:
        current.scan(
            jobs=args.jobs,
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
    progress = _Progress(args.progress)
    try:
        if args.map and (
            args.root_path
            or args.root_override
            or args.config
            or args.no_config
            or _has_discovery_overrides(args)
        ):
            raise ValueError("--map cannot be combined with repository discovery options")
        if args.map and args.jobs:
            raise ValueError("--jobs applies only to a live repository")
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
        if args.search_limit < 1 or args.search_limit > 100:
            raise ValueError("--search-limit must be from 1 to 100")
        if args.format == "json" and args.max_chars:
            raise ValueError("--max-chars applies only to Markdown")
        if args.format == "markdown" and args.pretty:
            raise ValueError("--pretty applies only to JSON")
        if args.compact and args.full:
            raise ValueError("--compact and --full conflict")
        if (args.compact or args.full) and args.detail != "standard":
            raise ValueError("--detail conflicts with --compact/--full")
        change_set = None
        if args.map:
            map_json = Path(args.map).read_bytes()
        else:
            if args.git_diff:
                repository, _, _ = _repository_inputs(args)
                change_set = _git_change_set(repository, args.git_diff)
            current = _project_from_args(args, progress)
            progress.emit({"phase": "rendering", "artifact": "canonical Map"})
            map_json = current.map_json()
            _warn_map_cache_stats(current.map_cache_stats)
        map_document = json.loads(map_json)
        if args.check and _has_error_diagnostics(map_document):
            return 1
        query_options = {
            "focus": args.focus,
            "paths": args.path,
            "symbols": args.symbol,
            "components": args.component,
            "packages": args.package,
            "artifacts": args.artifact,
            "search": args.search,
            "search_limit": args.search_limit,
            "change_set": change_set,
            "direction": args.direction,
            "producer_policy": (
                "current" if args.check and args.map else "compatible"
            ),
            "depth": args.depth,
            "test_depth": args.test_depth,
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
            query_options["context"] = context
        encoded = (
            query_map_json(map_json, pretty=args.pretty, **query_options)
            if args.format == "json"
            else query_map_markdown(
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
                **query_options,
            )
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


def _verify_main(argv: Sequence[str]) -> int:
    args = verification_parser().parse_args(argv)
    try:
        if args.root_path is not None and (
            args.config is not None or args.init is not None
        ):
            raise ValueError(
                "positional ROOT applies only when --config and --init are omitted"
            )
        if args.init is not None:
            if args.output == "-":
                raise ValueError(
                    "verify --init requires --output so relative paths stay portable"
                )
            if (
                args.project_root
                or args.baseline is not None
                or args.freeze is not None
                or args.freeze_owner is not None
                or args.freeze_rationale is not None
                or args.check
                or args.full
                or args.format != "json"
                or args.max_findings != 200
            ):
                raise ValueError(
                    "verify --init accepts only --init-root, --jobs, cache options, "
                    "--pretty, and --output"
                )
            config_path = Path(args.init).resolve()
            output_path = Path(args.output).resolve()
            try:
                relative_config = os.path.relpath(config_path, output_path.parent)
            except ValueError as error:
                raise ValueError(
                    "verify --init config and output must share a filesystem"
                ) from error
            if Path(relative_config).is_absolute():
                raise ValueError(
                    "verify --init refuses an absolute project config path"
                )
            project = Project.from_config(
                config_path,
                root=args.init_root,
                jobs=args.jobs,
                cache_dir=_cache_dir(args),
                cache_max_bytes=_cache_max_bytes(args),
            )
            encoded = draft_verification_suite(
                project,
                project_config=Path(relative_config).as_posix(),
                pretty=True,
            )
            _write(encoded, args.output)
            return 0
        if args.init_root is not None:
            raise ValueError("--init-root requires --init")
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
        suite_path = (
            Path(args.config)
            if args.config is not None
            else _discover_verification_suite(args.root_path)
        )
        baseline = args.baseline
        if baseline is None and args.freeze is not None and Path(args.freeze).is_file():
            baseline = args.freeze
        verification = Verification.from_config(
            suite_path,
            project_roots=_project_roots(args.project_root),
            baseline=baseline,
            jobs=args.jobs,
            cache_dir=_cache_dir(args),
            cache_max_bytes=_cache_max_bytes(args),
        )
        encoded = verification.report(
            args.format,
            full=args.full,
            max_findings=args.max_findings,
            pretty=args.pretty or args.format == "sarif",
        )
        _write(encoded, args.output)
        if args.freeze is not None:
            _write(
                verification.freeze(
                    owner=args.freeze_owner,
                    rationale=args.freeze_rationale,
                    pretty=True,
                ),
                args.freeze,
            )
        return int(args.check and verification.has_errors())
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _recipe_request(
    args: argparse.Namespace, *, project: Mapping[str, object]
) -> bytes:
    check: dict[str, object] = {
        "id": args.id,
        "owner": args.owner,
        "rationale": args.rationale,
        "severity": args.severity,
    }
    if args.requirement is not None:
        check["requirement"] = args.requirement
    if args.tag:
        check["tags"] = args.tag
    request = {
        "artifact": "verification-recipe-request",
        "schema_version": 1,
        "recipe": args.recipe,
        "project": dict(project),
        "arguments": {
            "max": args.max,
            "include": args.include,
            "exclude": args.exclude,
            "allow_empty": args.allow_empty,
        },
        "check": check,
    }
    return json.dumps(
        request,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _verification_recipe_main(argv: Sequence[str]) -> int:
    args = verification_recipe_parser().parse_args(argv)
    try:
        if args.command in {"list", "show"}:
            encoded = verification_recipe_catalog(
                "" if args.command == "list" else args.recipe,
                pretty=args.pretty,
            )
            _write(encoded, args.output)
            return 0
        if args.command == "compile":
            encoded = compile_verification_recipe(
                _recipe_request(args, project={"map": args.map_path}),
                pretty=args.pretty,
            )
            destination = Path(args.output)
            if args.output != "-" and os.path.lexists(destination) and not args.force:
                raise ConfigError(
                    f"refusing to replace existing verification suite: {destination}"
                )
            _write(encoded, args.output)
            return 0

        if args.jobs < 0:
            raise ValueError("--jobs must be zero or positive")
        if args.max_findings < 0:
            raise ValueError("--max-findings must be nonnegative")
        suite_json = compile_verification_recipe(
            _recipe_request(args, project={"map": "CURRENT.json"}),
            pretty=False,
        )
        plan = json.loads(verification_plan_json(suite_json))
        if len(plan["projects"]) != 1:
            raise RuntimeError("verification recipe must select exactly one project")
        repository, config_json, _ = _repository_inputs(args)
        progress = _Progress(args.progress)
        progress.emit({"phase": "discovery", "state": "start"})
        project = Project.from_repository(
            repository,
            config=config_json or None,
            project=args.project,
            scan=False,
            jobs=args.jobs,
        )
        progress.emit({"phase": "selected", "files": len(project.sources)})
        if plan["projects"][0]["provider_scan"]:
            project.scan(
                jobs=args.jobs,
                cache_dir=_cache_dir(args),
                cache_max_bytes=_cache_max_bytes(args),
                progress=progress.emit,
            )
        else:
            progress.emit({"phase": "joining", "state": "start"})
            project.finalize_providers()
            progress.emit({"phase": "joining", "state": "complete"})
        _warn_cache_stats(project.cache_stats)
        _warn_map_cache_stats(project.map_cache_stats)
        verification = Verification.from_map(
            suite_json,
            project.map_json(),
            resolution_json=project.resolution_json,
            path="recipe.verify.json",
        )
        progress.emit({"phase": "rendering", "artifact": "verification"})
        encoded = verification.report(
            args.format,
            full=args.full,
            max_findings=args.max_findings,
            pretty=args.pretty or args.format == "sarif",
        )
        _write(encoded, args.output)
        progress.finish()
        return int(args.check and verification.has_errors())
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _plan_main(argv: Sequence[str]) -> int:
    args = plan_parser().parse_args(argv)
    try:
        if args.max_candidates < 0:
            raise ValueError("--max-candidates must be nonnegative")
        encoded = change_proposal(
            Path(args.verification).read_bytes(),
            args.finding,
            format=args.format,
            full=args.full,
            max_candidates=args.max_candidates,
            pretty=args.pretty,
        )
        _write(encoded, args.output)
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _contract_main(argv: Sequence[str]) -> int:
    args = contract_parser().parse_args(argv)
    try:
        proposal_json = Path(args.proposal).read_bytes()
        preserve_checks = args.preserve_check
        if args.preserve_all:
            proposal_document = json.loads(proposal_json)
            preserve_checks = [
                str(row["id"])
                for row in proposal_document["preserved_invariants"]
            ]
        encoded = change_contract(
            proposal_json,
            objective=args.objective,
            owner=args.owner,
            rationale=args.rationale,
            preserve_checks=preserve_checks,
            selected_candidates=args.select_candidate,
            format=args.format,
            pretty=args.pretty,
        )
        _write(encoded, args.output)
        return 0
    except (ConfigError, OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"archbird: error: {error}", file=sys.stderr)
        return 2


def _verify_plan_main(argv: Sequence[str]) -> int:
    args = verify_plan_parser().parse_args(argv)
    try:
        proposal_json = Path(args.proposal).read_bytes()
        contract_json = Path(args.contract).read_bytes()
        before_json = Path(args.before_verification).read_bytes()
        after_json = Path(args.after_verification).read_bytes()
        encoded = change_verify(
            proposal_json,
            contract_json,
            before_json,
            after_json,
            format=args.format,
            pretty=args.pretty or args.format == "sarif",
        )
        _write(encoded, args.output)
        if not args.check:
            return 0
        result = json.loads(
            encoded
            if args.format == "json"
            else change_verify(
                proposal_json,
                contract_json,
                before_json,
                after_json,
                format="json",
                pretty=False,
            )
        )
        return int(result["status"] not in {"satisfied", "superseded"})
    except (ConfigError, OSError, RuntimeError, ValueError, KeyError) as error:
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
                proposal_path=args.proposal,
                contract_path=args.contract,
                result_path=args.result,
                replace=args.replace,
            )
            return 0
        if any((args.verification, args.proposal, args.contract, args.result)) or args.replace:
            raise ValueError(
                "verification/Act inputs and --replace apply only to export okf"
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


def main(argv: Optional[Sequence[str]] = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "map":
        arguments = arguments[1:]
    if arguments and arguments[0] == "query":
        return _query_main(arguments[1:], command="query", default_direction="both")
    if arguments and arguments[0] == "impact":
        return _query_main(
            arguments[1:], command="impact", default_direction="upstream"
        )
    if arguments and arguments[0] == "diff":
        return _diff_main(arguments[1:])
    if arguments and arguments[0] == "observe":
        return _observe_main(arguments[1:])
    if arguments and arguments[0] == "freshness":
        return _freshness_main(arguments[1:])
    if arguments and arguments[0] == "workspace":
        return _workspace_main(arguments[1:])
    if arguments and arguments[0] == "verify":
        if len(arguments) > 1 and arguments[1] == "recipe":
            return _verification_recipe_main(arguments[2:])
        return _verify_main(arguments[1:])
    if arguments and arguments[0] == "plan":
        return _plan_main(arguments[1:])
    if arguments and arguments[0] == "contract":
        return _contract_main(arguments[1:])
    if arguments and arguments[0] == "verify-plan":
        return _verify_plan_main(arguments[1:])
    if arguments and arguments[0] == "export":
        return _export_main(arguments[1:])
    if arguments and arguments[0] == "okf":
        return _okf_main(arguments[1:])
    if arguments and arguments[0] == "serve":
        return _serve_main(arguments[1:])
    if arguments and arguments[0] == "config":
        return _config_main(arguments[1:])
    if arguments and arguments[0] == "support":
        return _support_main(arguments[1:])
    args = parser().parse_args(arguments)
    progress = _Progress(args.progress)
    try:
        if args.max_chars < 0:
            raise ValueError("--max-chars must be nonnegative")
        if args.format == "json" and args.max_chars:
            raise ValueError("--max-chars applies only to Markdown")
        if args.format == "json" and (
            args.full or args.compact or args.detail != "standard" or args.view != "overview"
        ):
            raise ValueError("--view, --detail, --compact, and --full apply only to Markdown")
        if args.compact and args.full:
            raise ValueError("--compact and --full conflict")
        if args.compact and args.detail != "standard":
            raise ValueError("--compact conflicts with explicit --detail")
        if args.full and args.detail != "standard":
            raise ValueError("--full conflicts with explicit --detail")
        if args.format == "markdown" and args.pretty:
            raise ValueError("--pretty applies only to JSON")
        project = _project_from_args(args, progress)
        progress.emit({"phase": "rendering", "artifact": "canonical Map"})
        if args.format == "json" and not args.check:
            _write_project_map(project, args.output, pretty=args.pretty)
            progress.finish()
            return 0
        map_json = project.map_json(pretty=args.pretty and args.format == "json")
        _warn_map_cache_stats(project.map_cache_stats)
        document = json.loads(map_json)
        encoded = (
            map_json
            if args.format == "json"
            else render_map_markdown(
                map_json,
                view=args.view,
                detail="compact" if args.compact else args.detail,
                full=args.full,
                max_chars=args.max_chars,
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
