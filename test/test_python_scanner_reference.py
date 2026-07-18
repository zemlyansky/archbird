#!/usr/bin/env python3
"""Bound the portable Python lexical tier against CPython AST evidence."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys


def load_ast_provider(repository: Path):
    path = repository / "py/archbird/providers/python_ast.py"
    spec = importlib.util.spec_from_file_location("archbird_python_ast_probe", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load Python AST provider {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.python_ast_provider_facts


def occurrences(document: dict, domain: str) -> set[tuple[str, int, int]]:
    return {
        (row["name"], row["span"]["start"], row["span"]["end"])
        for row in document["facts"]
        if row["domain"] == domain
    }


def symbols(document: dict) -> set[tuple[str, str]]:
    return {
        (row["name"], row["kind"])
        for row in document["facts"]
        if row["domain"] == "symbols"
    }


def names(document: dict, domain: str) -> set[str]:
    return {
        row["name"] for row in document["facts"] if row["domain"] == domain
    }


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_python_scanner_reference.py SCANNER_CLI REPOSITORY ORACLE"
        )
    executable = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    oracle = Path(sys.argv[3]).resolve()
    ast_provider = load_ast_provider(repository)
    paths = sorted((repository / "py/archbird").rglob("*.py")) + sorted(
        (oracle / "archbird").rglob("*.py")
    )
    checked = 0
    missing_calls = 0
    for index, path in enumerate(paths):
        raw = path.read_bytes()
        text = raw.decode("utf-8")
        relative = f"probe/{index}/{path.name}"
        native = json.loads(
            subprocess.run(
                [str(executable), "python", relative],
                input=raw,
                capture_output=True,
                check=True,
            ).stdout
        )
        syntax = json.loads(
            ast_provider(
                project="scan",
                path=relative,
                text=text,
                source_manifest_sha256="0" * 64,
            )
        )
        if syntax["diagnostics"]:
            continue
        if symbols(native) != symbols(syntax):
            raise AssertionError(f"{path}: lexical symbol inventory differs")
        if names(native, "imports") != names(syntax, "imports"):
            raise AssertionError(f"{path}: lexical import inventory differs")
        for domain in ("calls", "method-calls"):
            lexical = occurrences(native, domain)
            parsed = occurrences(syntax, domain)
            unexpected = lexical - parsed
            if unexpected:
                raise AssertionError(
                    f"{path}: lexical {domain} contains unsupported occurrences "
                    f"{sorted(unexpected)!r}"
                )
            missing_calls += len(parsed - lexical)
        checked += 1
    if checked < 50:
        raise AssertionError(f"Python lexical differential covered only {checked} files")
    print(
        "native Python lexical/AST boundary passed: "
        f"{checked} files, {missing_calls} explicitly uncovered call occurrences"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
