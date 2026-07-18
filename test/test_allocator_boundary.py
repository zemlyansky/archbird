#!/usr/bin/env python3
"""Keep owned native allocation behind the engine allocator contract."""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import sys


CALL = re.compile(r"(?<![A-Za-z0-9_])(malloc|calloc|realloc|free)\s*\(")
DEFAULT_LINES = {
    "malloc": "return malloc(size);",
    "realloc": "return realloc(pointer, size);",
    "free": "free(pointer);",
}


def code_only(source: str) -> str:
    """Blank comments and quoted literals while preserving offsets/newlines."""
    output = list(source)
    state = "code"
    index = 0
    while index < len(source):
        byte = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if byte == "/" and following == "/":
                output[index] = output[index + 1] = " "
                index += 2
                state = "line-comment"
                continue
            if byte == "/" and following == "*":
                output[index] = output[index + 1] = " "
                index += 2
                state = "block-comment"
                continue
            if byte == '"':
                output[index] = " "
                index += 1
                state = "string"
                continue
            if byte == "'":
                output[index] = " "
                index += 1
                state = "character"
                continue
            index += 1
            continue
        if state == "line-comment":
            if byte == "\n":
                state = "code"
            else:
                output[index] = " "
            index += 1
            continue
        if state == "block-comment":
            if byte == "*" and following == "/":
                output[index] = output[index + 1] = " "
                index += 2
                state = "code"
            else:
                if byte != "\n":
                    output[index] = " "
                index += 1
            continue
        if byte == "\\" and following:
            output[index] = " "
            if following != "\n":
                output[index + 1] = " "
            index += 2
            continue
        if (state == "string" and byte == '"') or (
            state == "character" and byte == "'"
        ):
            output[index] = " "
            index += 1
            state = "code"
            continue
        if byte != "\n":
            output[index] = " "
        index += 1
    return "".join(output)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    engine_path = root / "src/base/engine.c"
    defaults: Counter[str] = Counter()
    violations: list[str] = []
    for path in sorted((root / "src").rglob("*.c")):
        source = path.read_text(encoding="utf-8")
        stripped = code_only(source)
        lines = source.splitlines()
        for match in CALL.finditer(stripped):
            name = match.group(1)
            line_number = stripped.count("\n", 0, match.start()) + 1
            line = lines[line_number - 1].strip()
            relative = path.relative_to(root).as_posix()
            if path == engine_path and line == DEFAULT_LINES[name]:
                defaults[name] += 1
            else:
                violations.append(f"{relative}:{line_number}: {line}")
    expected = Counter({"malloc": 1, "realloc": 1, "free": 1})
    if defaults != expected:
        violations.append(
            f"src/base/engine.c: default libc calls {dict(defaults)} != {dict(expected)}"
        )
    if violations:
        print("direct owned libc allocation bypasses the engine contract:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1
    print("native allocator boundary passed: only engine defaults call libc")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
