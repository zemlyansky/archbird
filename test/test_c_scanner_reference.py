#!/usr/bin/env python3
"""Compare native C provider facts with the immutable schema-6 scanner."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import subprocess
import sys

from archbird.map.scanners import c_file_facts


CASES = (
    (
        "definitions",
        "c",
        "/* fake(); */\nstatic int helper(int x) { return x + 1; }\n"
        "int api(int x) { return helper(x); }\n",
        {"api"},
    ),
    (
        "prototypes",
        "c",
        "int api(int x);\nstatic int helper(int x);\n",
        {"api"},
    ),
    (
        "extern-c",
        "cpp",
        '#ifdef __cplusplus\nextern "C" {\n#endif\nint api(int x);\n'
        "#ifdef __cplusplus\n}\n#endif\n",
        {"api"},
    ),
    (
        "typedef-call",
        "c",
        "typedef int (*handler_t)(int);\n"
        "int run(int value) { handler_t(value); return actual(value); }\n",
        set(),
    ),
    (
        "napi",
        "c",
        'static napi_property_descriptor rows[] = {{"run", NULL, napi_run, NULL}};\n'
        'DECLARE_NAPI_METHOD("stop", napi_stop);\n',
        set(),
    ),
    (
        "declaration-not-call",
        "c",
        "typedef unsigned long word_t;\nword_t build(word_t x);\n"
        "word_t invoke(word_t x) { return build(x); }\n",
        {"build"},
    ),
)


def project(document: dict) -> tuple[list[tuple], set[str], dict[str, int]]:
    symbols = []
    exports: set[str] = set()
    calls: Counter[str] = Counter()
    for fact in document["facts"]:
        if fact["domain"] == "symbols":
            attributes = fact.get("attributes", {})
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
        raise SystemExit("usage: test_c_scanner_reference.py SCANNER_CLI")
    executable = Path(sys.argv[1]).resolve()
    for name, language, source, public_names in CASES:
        command = [str(executable), language, f"src/{name}.c", *sorted(public_names)]
        first = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        second = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        if first.stdout != second.stdout:
            raise AssertionError(f"{name}: provider bytes are not deterministic")
        documents = [json.loads(line) for line in first.stdout.splitlines()]
        subject = next(
            row for row in documents if row["subject"]["path"] == f"src/{name}.c"
        )
        native = project(subject)
        oracle = c_file_facts(
            f"src/{name}.c", "core", language, source, "0" * 64, public_names
        )
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
                f"{name}: native C projection differs\n"
                f"native={native!r}\noracle={wanted!r}"
            )
    print(f"native C scanner parity passed: {len(CASES)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
