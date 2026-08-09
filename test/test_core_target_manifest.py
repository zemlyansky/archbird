#!/usr/bin/env python3
"""Exercise coarse native target partition and dependency drift failures."""

from __future__ import annotations

from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.core_source_manifest import MANIFEST_PATH
from tools.core_target_manifest import (
    TARGET_MANIFEST_PATH,
    TargetManifestError,
    validate_target_manifest,
)


def expect_failure(callable_, fragment: str) -> None:
    try:
        callable_()
    except TargetManifestError as error:
        if fragment not in str(error):
            raise AssertionError(
                f"expected failure containing {fragment!r}, got {error!r}"
            ) from error
    else:
        raise AssertionError(
            f"expected target manifest failure containing {fragment!r}"
        )


def copy_fixture(repository: Path, destination: Path) -> None:
    destination.mkdir()
    shutil.copy2(repository / "archbird.json", destination / "archbird.json")
    (destination / "cmake").mkdir()
    shutil.copy2(repository / MANIFEST_PATH, destination / MANIFEST_PATH)
    shutil.copy2(
        repository / TARGET_MANIFEST_PATH,
        destination / TARGET_MANIFEST_PATH,
    )
    shutil.copytree(repository / "src", destination / "src")


def replace_once(path: Path, before: str, after: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(before) != 1:
        raise AssertionError(
            f"expected exactly one {before!r} in {path}, found {text.count(before)}"
        )
    path.write_text(text.replace(before, after), encoding="utf-8")


def main() -> int:
    repository = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    inventory = validate_target_manifest(repository)
    if len(inventory.groups) != 10 or not inventory.dependency_edges:
        raise AssertionError("coarse native target inventory is unexpectedly empty")

    build_root = repository / "build"
    build_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="core-target-manifest-", dir=build_root
    ) as raw:
        root = Path(raw)

        unassigned = root / "unassigned"
        copy_fixture(repository, unassigned)
        replace_once(
            unassigned / TARGET_MANIFEST_PATH,
            "set(ARCHBIRD_CORE_GROUP_foundation_SOURCE_PREFIXES\n  base\n)",
            "set(ARCHBIRD_CORE_GROUP_foundation_SOURCE_PREFIXES\n  absent\n)",
        )
        expect_failure(
            lambda: validate_target_manifest(unassigned),
            "src/base/artifact_validation.c: expected exactly one coarse native "
            "target, found none",
        )

        overlapping = root / "overlapping"
        copy_fixture(repository, overlapping)
        replace_once(
            overlapping / TARGET_MANIFEST_PATH,
            "set(ARCHBIRD_CORE_GROUP_model_SOURCE_PREFIXES\n  projection",
            "set(ARCHBIRD_CORE_GROUP_model_SOURCE_PREFIXES\n  base\n  projection",
        )
        expect_failure(
            lambda: validate_target_manifest(overlapping),
            "source prefix 'base' is repeated by foundation and model",
        )

        missing_edge = root / "missing-edge"
        copy_fixture(repository, missing_edge)
        replace_once(
            missing_edge / TARGET_MANIFEST_PATH,
            "set(ARCHBIRD_CORE_GROUP_mapping_DEPENDENCIES\n"
            "  evidence\n  foundation\n)",
            "set(ARCHBIRD_CORE_GROUP_mapping_DEPENDENCIES\n  foundation\n)",
        )
        expect_failure(
            lambda: validate_target_manifest(missing_edge),
            "missing=mapping->evidence",
        )

        extra_edge = root / "extra-edge"
        copy_fixture(repository, extra_edge)
        replace_once(
            extra_edge / TARGET_MANIFEST_PATH,
            "set(ARCHBIRD_CORE_GROUP_api_DEPENDENCIES\n"
            "  configuration\n  foundation\n  interchange\n  model\n",
            "set(ARCHBIRD_CORE_GROUP_api_DEPENDENCIES\n"
            "  configuration\n  foundation\n  interchange\n  mapping\n  model\n",
        )
        expect_failure(
            lambda: validate_target_manifest(extra_edge),
            "unexpected=api->mapping",
        )

        later_edge = root / "later-edge"
        copy_fixture(repository, later_edge)
        replace_once(
            later_edge / TARGET_MANIFEST_PATH,
            "set(ARCHBIRD_CORE_GROUP_foundation_DEPENDENCIES\n)",
            "set(ARCHBIRD_CORE_GROUP_foundation_DEPENDENCIES\n  api\n)",
        )
        expect_failure(
            lambda: validate_target_manifest(later_edge),
            "foundation depends on unknown or later group api",
        )

    print(
        "native core target manifest regression contract passed: "
        f"{len(inventory.groups)} groups, "
        f"{len(inventory.source_groups)} sources, "
        f"{len(inventory.header_groups)} headers, "
        f"{len(inventory.dependency_edges)} dependencies"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
