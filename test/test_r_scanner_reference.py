#!/usr/bin/env python3
"""Compare native R provider facts with the immutable schema-6 scanner."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import subprocess
import sys

from archbird.map.scanners import r_file_facts


CASES = (
    (
        "functions",
        "foo <- function(x) .Call(poly_run, x)\n"
        ".hidden = function() other()\n",
    ),
    (
        "comments",
        "# fake <- function() fake_call()\n"
        "real = function(x) { # ignored()\n  visible(x)\n}\n",
    ),
    (
        "names",
        "foo.bar <- function() pkg::run()\n"
        "under_score = function() .Call(.native_call)\n",
    ),
)


def project(document: dict) -> tuple[list[tuple], set[str], dict[str, int]]:
    symbols = []
    exports: set[str] = set()
    calls: Counter[str] = Counter()
    for fact in document["facts"]:
        if fact["domain"] == "symbols":
            attributes = fact["attributes"]
            symbols.append(
                (
                    attributes["line"],
                    fact["name"],
                    fact["kind"],
                    attributes["scope"],
                    attributes["signature"],
                )
            )
        elif fact["domain"] == "calls":
            calls[fact["name"]] += 1
        elif fact["domain"] == "exports":
            exports.add(fact["name"])
    return sorted(set(symbols)), exports, dict(sorted(calls.items()))


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_r_scanner_reference.py SCANNER_CLI")
    executable = Path(sys.argv[1]).resolve()
    for name, source in CASES:
        command = [str(executable), "r", f"R/{name}.R"]
        first = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        second = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        if first.stdout != second.stdout:
            raise AssertionError(f"{name}: provider bytes are not deterministic")
        document = json.loads(first.stdout)
        native = project(document)
        oracle = r_file_facts(f"R/{name}.R", "r", source, "0" * 64)
        wanted = (
            sorted(
                {
                    (
                        symbol.line,
                        symbol.name,
                        symbol.kind,
                        symbol.scope,
                        symbol.signature,
                    )
                    for symbol in oracle.symbols
                }
            ),
            oracle.exports,
            oracle.call_counts,
        )
        if native != wanted:
            raise AssertionError(
                f"{name}: native R projection differs\n"
                f"native={native!r}\noracle={wanted!r}"
            )
    print(f"native R scanner parity passed: {len(CASES)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
