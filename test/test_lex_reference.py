#!/usr/bin/env python3
"""Compare native byte-offset tokenization with a maintainer reference."""

from __future__ import annotations

from pathlib import Path
import random
import subprocess
import sys

from archbird.map.scanners import tokenize


NAMED = (
    ("plain", "alpha(1, 'x');", False, False),
    ("c-preprocessor", "  #define fake() 1\nint real(void);\n", True, False),
    ("comments", "a/* one\ntwo */+b// tail\n-c", False, False),
    ("operators", "a===b !== c >>> 2; x?.y ?? z ** 2", False, True),
    ("numbers", "0 01 0x1f 0x 1. 1.25 1e3 2E-4 3e+", False, False),
    ("strings", "'a\\\'b' \"c\\\"d\" `e\\`f`", False, True),
    ("js-regex", r"input.replace(/https?:\/\//g, ''); left / right;", False, True),
    ("regex-class", r"return /[/\\]]+/gi; value / divisor", False, True),
    ("unicode-string", "call('π🐦'); next()", False, False),
    ("multiline-template", "`one\ntwo`; done()", False, True),
    ("extern-c", '#ifdef X\nextern "C" {\n#endif\nint api();\n', True, False),
)


def expected(source: str, c_preprocessor: bool, javascript: bool) -> bytes:
    rows = []
    for token in tokenize(
        source, c_preprocessor=c_preprocessor, javascript=javascript
    ):
        start = len(source[: token.start].encode("utf-8"))
        end = len(source[: token.end].encode("utf-8"))
        rows.append(
            f"{start}\t{end}\t{token.line}\t{token.kind}\t"
            f"{token.value.encode('utf-8').hex()}\n"
        )
    return "".join(rows).encode("ascii")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_lex_reference.py LEX_CLI")
    executable = Path(sys.argv[1]).resolve()
    cases = list(NAMED)
    rng = random.Random(0xA7C4B1D)
    atoms = [
        "name",
        "call()",
        "// comment\n",
        "/* block */",
        "123.5e-2",
        "'text'",
        "x=>x+1",
        "[a,b]",
        "{k:v}",
        "π",
    ]
    for index in range(64):
        source = " ".join(rng.choice(atoms) for _ in range(rng.randint(1, 12)))
        cases.append((f"seeded-{index}", source, False, bool(index % 2)))
    for name, source, c_preprocessor, javascript in cases:
        command = [str(executable)]
        if c_preprocessor:
            command.append("--c")
        elif javascript:
            command.append("--js")
        first = subprocess.run(
            command, input=source.encode("utf-8"), capture_output=True, check=True
        )
        second = subprocess.run(
            command, input=source.encode("utf-8"), capture_output=True, check=True
        )
        wanted = expected(source, c_preprocessor, javascript)
        if first.stdout != wanted:
            raise AssertionError(
                f"{name}: native token projection differs\n"
                f"native={first.stdout!r}\noracle={wanted!r}"
            )
        if second.stdout != first.stdout:
            raise AssertionError(f"{name}: native token output is not deterministic")
    print(f"native tokenizer parity passed: {len(cases)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
