#!/usr/bin/env python3
"""Keep the vendored JSON parser at the native interchange boundary."""

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {
    Path("src/base/json.c"),
    Path("src/base/json_internal.h"),
    Path("src/base/json_number.c"),
    Path("src/base/json_value.c"),
    Path("src/evidence/evidence.c"),
    Path("src/evidence/config_json.c"),
    Path("src/map/package_json.c"),
}
NEEDLES = ("yyjson_", '"yyjson.h"')
PATTERN_ALLOWED = {Path("src/base/pattern.c")}
PATTERN_NEEDLES = ("pcre2_", '"pcre2.h"')


def main() -> int:
    leaks: list[str] = []
    for path in sorted((ROOT / "src").rglob("*.[ch]")):
        relative = path.relative_to(ROOT)
        if relative in ALLOWED:
            continue
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if any(needle in line for needle in NEEDLES):
                leaks.append(f"{relative}:{line_number}: {line.strip()}")
    if leaks:
        print("yyjson escaped the JSON/evidence decode boundary:", file=sys.stderr)
        print("\n".join(leaks), file=sys.stderr)
        return 1
    pattern_leaks: list[str] = []
    for path in sorted((ROOT / "src").rglob("*.[ch]")):
        relative = path.relative_to(ROOT)
        if relative in PATTERN_ALLOWED:
            continue
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if any(needle in line for needle in PATTERN_NEEDLES):
                pattern_leaks.append(
                    f"{relative}:{line_number}: {line.strip()}"
                )
    if pattern_leaks:
        print("PCRE2 escaped the pattern boundary:", file=sys.stderr)
        print("\n".join(pattern_leaks), file=sys.stderr)
        return 1
    print(
        "parser boundaries: yyjson in 7 JSON/decode files, PCRE2 in pattern.c; "
        "no typed-core leaks"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
