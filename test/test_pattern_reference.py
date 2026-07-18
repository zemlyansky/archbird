#!/usr/bin/env python3
"""PCRE2 pattern-contract tests and Python-0.10 migration evidence.

PCRE2 is the only parser that defines native Archbird pattern semantics.  The
Python ``re`` calls below are a differential migration ledger for configurations
created by the immutable Python implementation; they do not constrain the
native grammar to Python's grammar.
"""

from __future__ import annotations

import random
import re
import subprocess
import sys
from typing import Iterable, Optional


# Constructs intentionally common to CPython 3.10 ``re`` and
# archbird-pcre2-v1. These protect existing profiles during migration without
# pretending that the two complete dialects are identical.
COMMON_MATCH_CASES = (
    (r"libarchbird\.a", "build/libarchbird.a then libarchbird.a", 0),
    (r"(archbird_[A-Za-z0-9_]+)", "archbird_map + archbird_verify", 1),
    (r"_?(poly_[A-Za-z0-9_]+)", "_poly_add poly_mul", 1),
    (r'\{"(C_poly_[A-Za-z0-9_]+)"\s*,\s*\(DL_FUNC\)', '{"C_poly_add", (DL_FUNC) fn}', 1),
    (r"/dist/polygrad\.sync\.js", "load /dist/polygrad.sync.js", 0),
    (r"core\.sync\.js", "core.sync.js coreXsyncXjs", 0),
    (r"^(codec|ops|io)\.", "ops.add", 1),
    (r"\b\w+\b", "café κόσμος", 0),
    (r"(?=a)", "aa", 0),
    (r"(a)?b", "b ab", 1),
    (r"(?P<word>[A-Za-z]+)-(?P=word)", "go-go no-stop", 1),
    (r"(?m)^item$", "no\nitem\nyes", 0),
    (r"(?s)a.*?z", "a\nmid\nz", 0),
    (r"(?<=prefix:)\w+", "prefix:value", 0),
    (r"(?:foo|bar){1,2}", "foobar baz barbar", 0),
)


# Exact native behavior for useful PCRE2 syntax and pinned UCD semantics. None
# of these cases is derived from Python's parser.
NATIVE_CASES = (
    (r"[[:alpha:]]+", "abc", ((0, 3, 0, 0, 0),)),
    (r"\z", "a", ((1, 1, 0, 0, 0),)),
    (r"\Qliteral\E", "literal", ((0, 7, 0, 0, 0),)),
    (r"(?>a*)", "aaa", ((0, 3, 0, 0, 0), (3, 3, 0, 0, 0))),
    (r"a*+", "aaa", ((0, 3, 0, 0, 0), (3, 3, 0, 0, 0))),
    (r"(*UTF)a", "a", ((0, 1, 0, 0, 0),)),
    (r"\B", "", ((0, 0, 0, 0, 0),)),
    # Nag Mundari letter U+1E4D0 entered Unicode in 15.0. The bundled UCD 16.0
    # must classify it as a word character regardless of the host Python UCD.
    (r"\w", "\U0001E4D0", ((0, 4, 0, 0, 0),)),
)


# Stable, intentional Python-0.10 -> native differences. Each row freezes both
# sides of the migration observation. A future Python runtime can be added as a
# separate observation rather than changing the native contract.
MIGRATION_DIFFERENCES = (
    (r"a\Z", "a\n", (), ((0, 1, 0, 0, 0),)),
    (r"\u0041", "A", ((0, 1, 0, 0, 0),), None),
    (r"\N{LATIN SMALL LETTER A}", "a", ((0, 1, 0, 0, 0),), None),
    (r"\p{L}", "a", None, ((0, 1, 0, 0, 0),)),
    (r"(?|a|b)", "b", None, ((0, 1, 0, 0, 0),)),
    (r"\w", "\u0301", (), ((0, 2, 0, 0, 0),)),
    (r"\s", "\x1c", ((0, 1, 0, 0, 0),), ()),
    (r"\s", "\u180e", (), ((0, 3, 0, 0, 0),)),
    (r"(?i)i", "\u0130", ((0, 2, 0, 0, 0),), ()),
    (r"(?<=a|bc)x", "bcx", None, ((2, 3, 0, 0, 0),)),
    (r"\400", "\u0100", None, ((0, 2, 0, 0, 0),)),
)


# These are the only syntax-level exclusions added by Archbird. They are set
# through PCRE2 options, not recognized by another parser: callouts could invoke
# application callbacks, and ``\C`` can split UTF-8 code points into invalid
# evidence spans.
SECURITY_REJECTIONS = (
    (r"(?C)a", "using callouts is disabled by the application"),
    (r"\C", r"using \C is disabled by the application"),
)


def byte_offset(text: str, offset: int) -> int:
    return len(text[:offset].encode("utf-8"))


def python_rows(pattern: str, subject: str) -> Optional[tuple[tuple[int, ...], ...]]:
    try:
        matches = re.finditer(pattern, subject)
        return tuple(
            (
                byte_offset(subject, match.start()),
                byte_offset(subject, match.end()),
                0,
                0,
                0,
            )
            for match in matches
        )
    except re.error:
        return None


def python_expected(
    pattern: str, subject: str, capture: int
) -> tuple[tuple[int, ...], ...]:
    rows = []
    for match in re.finditer(pattern, subject):
        start, end = match.span()
        capture_span = match.span(capture) if capture else (-1, -1)
        present = capture > 0 and capture_span != (-1, -1)
        rows.append(
            (
                byte_offset(subject, start),
                byte_offset(subject, end),
                int(present),
                byte_offset(subject, capture_span[0]) if present else 0,
                byte_offset(subject, capture_span[1]) if present else 0,
            )
        )
    return tuple(rows)


def run_native(
    executable: str, pattern: str, subject: str, capture: int = 0
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, str(capture), pattern, subject],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def native_rows(
    executable: str, pattern: str, subject: str, capture: int = 0
) -> tuple[tuple[int, ...], ...]:
    result = run_native(executable, pattern, subject, capture)
    if result.returncode:
        raise AssertionError((pattern, subject, result.returncode, result.stderr))
    return tuple(tuple(map(int, line.split())) for line in result.stdout.splitlines())


def random_cases(seed: int = 0xA7C8B1, count: int = 512) -> Iterable[tuple[str, str, int]]:
    randomizer = random.Random(seed)
    atoms = (
        "a",
        "b",
        "c",
        ".",
        r"\d",
        r"\s",
        r"\w",
        "[abc]",
        "[^x]",
        "(?:ab|c)",
        "(?:a|b)",
    )
    suffixes = ("", "?", "*", "+", "{1,3}", "??", "*?", "+?")
    alphabet = "abcXYZ012 _-\né"
    for _ in range(count):
        pattern = "".join(
            randomizer.choice(atoms) + randomizer.choice(suffixes)
            for _ in range(randomizer.randint(1, 4))
        )
        subject = "".join(
            randomizer.choice(alphabet) for _ in range(randomizer.randint(0, 12))
        )
        yield pattern, subject, 0


def assert_common(executable: str, cases: Iterable[tuple[str, str, int]]) -> int:
    count = 0
    for pattern, subject, capture in cases:
        wanted = python_expected(pattern, subject, capture)
        found = native_rows(executable, pattern, subject, capture)
        assert found == wanted, (pattern, subject, wanted, found)
        count += 1
    return count


def assert_native(executable: str) -> int:
    for pattern, subject, wanted in NATIVE_CASES:
        found = native_rows(executable, pattern, subject)
        assert found == wanted, (pattern, subject, wanted, found)
    return len(NATIVE_CASES)


def assert_migration_differences(executable: str) -> int:
    for pattern, subject, wanted_python, wanted_native in MIGRATION_DIFFERENCES:
        found_python = python_rows(pattern, subject)
        assert found_python == wanted_python, (
            pattern,
            subject,
            wanted_python,
            found_python,
            sys.version,
        )
        result = run_native(executable, pattern, subject)
        if wanted_native is None:
            assert result.returncode != 0, (pattern, result.stdout)
            assert "archbird-pcre2-v1" in result.stderr, (pattern, result.stderr)
        else:
            assert result.returncode == 0, (pattern, result.stderr)
            found_native = tuple(
                tuple(map(int, line.split())) for line in result.stdout.splitlines()
            )
            assert found_native == wanted_native, (
                pattern,
                subject,
                wanted_native,
                found_native,
            )
    return len(MIGRATION_DIFFERENCES)


def assert_security_rejections(executable: str) -> int:
    for pattern, message in SECURITY_REJECTIONS:
        result = run_native(executable, pattern, "a")
        assert result.returncode != 0, (pattern, result.stdout)
        assert "archbird-pcre2-v1" in result.stderr, (pattern, result.stderr)
        assert message in result.stderr, (pattern, message, result.stderr)
    return len(SECURITY_REJECTIONS)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_pattern_reference.py PATTERN_CLI")
    executable = sys.argv[1]
    common = assert_common(executable, COMMON_MATCH_CASES)
    randomized = assert_common(executable, random_cases())
    native = assert_native(executable)
    migration = assert_migration_differences(executable)
    rejected = assert_security_rejections(executable)
    print(
        "archbird-pcre2-v1 passed: "
        f"{common} curated common, {randomized} seeded common, "
        f"{native} native, {migration} migration-difference, "
        f"{rejected} security-rejection cases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
