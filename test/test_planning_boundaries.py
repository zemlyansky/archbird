#!/usr/bin/env python3
"""Enforce project-configuration orchestration at the public API boundary."""

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
DOMAINS = ("query", "constraints")
FORBIDDEN = (
    "project_configuration.h",
    "AbProjectConfiguration",
    "ab_project_configuration_",
)


def main() -> int:
    leaks: list[str] = []
    for domain in DOMAINS:
        for path in sorted((ROOT / "src" / domain).glob("*.[ch]")):
            relative = path.relative_to(ROOT)
            for line_number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1
            ):
                if any(needle in line for needle in FORBIDDEN):
                    leaks.append(f"{relative}:{line_number}: {line.strip()}")
    if leaks:
        print(
            "project configuration escaped the API orchestration boundary:",
            file=sys.stderr,
        )
        print("\n".join(leaks), file=sys.stderr)
        return 1

    constraint_interface = (
        ROOT / "src/constraints/constraints_internal.h"
    ).read_text(encoding="utf-8")
    if "const AbValue *project_configuration;" in constraint_interface:
        print(
            "constraint policy input exposes the complete project configuration",
            file=sys.stderr,
        )
        return 1

    required = {
        ROOT / "src/api/query_plan.c": (
            "ab_project_configuration_decode",
            "ab_query_plan_compile_definition",
        ),
        ROOT / "src/api/constraints.c": (
            "ab_project_configuration_decode",
            "ab_constraints_evaluate",
        ),
    }
    for path, needles in required.items():
        text = path.read_text(encoding="utf-8")
        missing = [needle for needle in needles if needle not in text]
        if missing:
            print(
                f"{path.relative_to(ROOT)} does not compose {', '.join(missing)}",
                file=sys.stderr,
            )
            return 1

    print(
        "planning boundaries: API composes project configuration with Query "
        "and Constraints; domain planners do not decode it"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
