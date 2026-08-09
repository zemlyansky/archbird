#!/usr/bin/env python3
"""Exercise native-core manifest completeness and ownership drift failures."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.core_source_manifest import MANIFEST_PATH, ManifestError


def expect_failure(callable_, fragment: str) -> None:
    try:
        callable_()
    except ManifestError as error:
        if fragment not in str(error):
            raise AssertionError(
                f"expected failure containing {fragment!r}, got {error!r}"
            ) from error
    else:
        raise AssertionError(f"expected manifest failure containing {fragment!r}")


def copy_fixture(repository: Path, destination: Path) -> None:
    destination.mkdir()
    shutil.copy2(repository / "archbird.json", destination / "archbird.json")
    (destination / "cmake").mkdir()
    shutil.copy2(
        repository / MANIFEST_PATH,
        destination / MANIFEST_PATH,
    )
    shutil.copytree(repository / "src", destination / "src")


def main() -> int:
    repository = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    sys.path.insert(0, str(repository))
    from tools.core_source_manifest import validate_manifest

    entries = validate_manifest(repository)
    if not entries or any(not entry.component for entry in entries):
        raise AssertionError("validated native core manifest is unexpectedly empty")

    build_root = repository / "build"
    build_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="core-source-manifest-", dir=build_root
    ) as raw:
        root = Path(raw)

        unlisted = root / "unlisted"
        copy_fixture(repository, unlisted)
        (unlisted / "src" / "base" / "unlisted.c").write_text(
            "/* deliberately absent from the manifest */\n", encoding="ascii"
        )
        expect_failure(
            lambda: validate_manifest(unlisted),
            "unlisted native source(s): src/base/unlisted.c",
        )

        duplicate = root / "duplicate"
        copy_fixture(repository, duplicate)
        manifest = duplicate / MANIFEST_PATH
        rows = manifest.read_text(encoding="utf-8").splitlines()
        first_entry = next(row for row in rows if row.strip().startswith("src/"))
        rows.insert(rows.index(")"), first_entry)
        manifest.write_text("\n".join((*rows, "")), encoding="utf-8")
        expect_failure(lambda: validate_manifest(duplicate), "duplicate native source")

        wrong_owner = root / "wrong-owner"
        copy_fixture(repository, wrong_owner)
        configuration_path = wrong_owner / "archbird.json"
        configuration = json.loads(configuration_path.read_text(encoding="utf-8"))
        base = next(
            component
            for component in configuration["components"]
            if component["name"] == "base"
        )
        base["paths"].append("src/act/act_accept.c")
        configuration_path.write_text(
            json.dumps(configuration, indent=2) + "\n", encoding="utf-8"
        )
        expect_failure(
            lambda: validate_manifest(wrong_owner),
            "expected exactly one reviewed archbird.json component, found act, base",
        )

        missing = root / "missing"
        copy_fixture(repository, missing)
        (missing / "src" / "act" / "act_accept.c").unlink()
        expect_failure(
            lambda: validate_manifest(missing),
            "listed native source does not exist: src/act/act_accept.c",
        )

        unsorted = root / "unsorted"
        copy_fixture(repository, unsorted)
        manifest = unsorted / MANIFEST_PATH
        rows = manifest.read_text(encoding="utf-8").splitlines()
        indexes = [
            index for index, row in enumerate(rows) if row.strip().startswith("src/")
        ]
        rows[indexes[0]], rows[indexes[1]] = rows[indexes[1]], rows[indexes[0]]
        manifest.write_text("\n".join((*rows, "")), encoding="utf-8")
        expect_failure(
            lambda: validate_manifest(unsorted),
            "native source manifest must be sorted by source path",
        )

    print("native core source manifest regression contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
