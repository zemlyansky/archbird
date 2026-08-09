#!/usr/bin/env python3
"""Exercise private include qualification and component-edge failures."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.private_include_boundaries import (
    IncludeBoundaryError,
    validate_private_includes,
)


def expect_failure(callable_, fragment: str) -> None:
    try:
        callable_()
    except IncludeBoundaryError as error:
        if fragment not in str(error):
            raise AssertionError(
                f"expected failure containing {fragment!r}, got {error!r}"
            ) from error
    else:
        raise AssertionError(
            f"expected private include failure containing {fragment!r}"
        )


def copy_fixture(repository: Path, destination: Path) -> None:
    destination.mkdir()
    shutil.copy2(repository / "archbird.json", destination / "archbird.json")
    shutil.copytree(repository / "src", destination / "src")
    shutil.copytree(repository / "bindings", destination / "bindings")
    shutil.copytree(
        repository / "test",
        destination / "test",
        ignore=shutil.ignore_patterns("fixtures"),
    )


def replace_once(path: Path, before: str, after: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(before) != 1:
        raise AssertionError(
            f"expected exactly one {before!r} in {path}, found {text.count(before)}"
        )
    path.write_text(text.replace(before, after), encoding="utf-8")


def main() -> int:
    repository = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    inventory = validate_private_includes(repository)
    if inventory.header_count <= 0 or not inventory.relations:
        raise AssertionError("private include inventory is unexpectedly empty")

    build_root = repository / "build"
    build_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="private-include-boundaries-", dir=build_root
    ) as raw:
        root = Path(raw)

        unqualified = root / "unqualified"
        copy_fixture(repository, unqualified)
        replace_once(
            unqualified / "src/base/json_value.c",
            '#include "base/json_value.h"',
            '#include "json_value.h"',
        )
        expect_failure(
            lambda: validate_private_includes(unqualified),
            "private include must be module-qualified from src/",
        )

        ambiguous = root / "ambiguous"
        copy_fixture(repository, ambiguous)
        replace_once(
            ambiguous / "src/evidence/semantic/scip/scanner.c",
            '#include "evidence/semantic/scip/scanner.h"',
            '#include "scanner.h"',
        )
        expect_failure(
            lambda: validate_private_includes(ambiguous),
            "src/evidence/lexical/c/scanner.h",
        )

        escape = root / "escape"
        copy_fixture(repository, escape)
        replace_once(
            escape / "src/base/json_value.c",
            '#include "base/json_value.h"',
            '#include "../base/json_value.h"',
        )
        expect_failure(
            lambda: validate_private_includes(escape),
            "native include escape is forbidden",
        )

        angle = root / "angle"
        copy_fixture(repository, angle)
        replace_once(
            angle / "src/base/json_value.c",
            '#include "base/json_value.h"',
            "#include <base/json_value.h>",
        )
        expect_failure(
            lambda: validate_private_includes(angle),
            "private include must use quotes",
        )

        macro = root / "macro"
        copy_fixture(repository, macro)
        replace_once(
            macro / "src/base/json_value.c",
            '#include "base/json_value.h"',
            "#define AB_PRIVATE_HEADER \"base/json_value.h\"\n"
            "#include AB_PRIVATE_HEADER",
        )
        expect_failure(
            lambda: validate_private_includes(macro),
            "include directives must use literal header paths",
        )

        prefixed = root / "prefixed"
        copy_fixture(repository, prefixed)
        replace_once(
            prefixed / "src/base/json_value.c",
            '#include "base/json_value.h"',
            '#include "src/base/json_value.h"',
        )
        expect_failure(
            lambda: validate_private_includes(prefixed),
            "private include must omit the src/ prefix",
        )

        forbidden = root / "forbidden"
        copy_fixture(repository, forbidden)
        model = forbidden / "src/base/model.c"
        model.write_text(
            '#include "evidence/evidence.h"\n' + model.read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        expect_failure(
            lambda: validate_private_includes(forbidden),
            "forbidden private include edge base->evidence",
        )

        wrong_owner = root / "wrong-owner"
        copy_fixture(repository, wrong_owner)
        configuration_path = wrong_owner / "archbird.json"
        configuration = json.loads(configuration_path.read_text(encoding="utf-8"))
        base = next(
            component
            for component in configuration["components"]
            if component["name"] == "base"
        )
        base["paths"].append("src/evidence/evidence.h")
        configuration_path.write_text(
            json.dumps(configuration, indent=2) + "\n", encoding="utf-8"
        )
        expect_failure(
            lambda: validate_private_includes(wrong_owner),
            "expected exactly one reviewed archbird.json component, "
            "found base, evidence",
        )

    print(
        "private include boundary regression contract passed: "
        f"{inventory.header_count} headers, {len(inventory.relations)} relations, "
        f"{len(inventory.component_edges)} cross-component edges"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
