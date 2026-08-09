#!/usr/bin/env python3
"""Validate explicit native-core build inputs against reviewed components."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import fnmatch
import json
from pathlib import Path, PurePosixPath
import re
import sys


MANIFEST_PATH = Path("cmake/ArchbirdCoreSources.cmake")
PROJECT_CONFIGURATION_PATH = Path("archbird.json")
_COMPONENT_RE = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")
_SOURCE_RE = re.compile(r"src/[A-Za-z0-9_./+-]+\.c\Z")
_MANIFEST_START = "set(ARCHBIRD_DECLARED_CORE_SOURCE_PATHS"


class ManifestError(ValueError):
    """The native-core source manifest is malformed or stale."""


@dataclass(frozen=True)
class SourceEntry:
    component: str
    path: str


def _validate_source_path(path: str, *, line_number: int) -> None:
    candidate = PurePosixPath(path)
    if (
        not _SOURCE_RE.fullmatch(path)
        or path != candidate.as_posix()
        or candidate.is_absolute()
        or ".." in candidate.parts
        or "\\" in path
    ):
        raise ManifestError(
            f"line {line_number}: native source must be a canonical "
            f"repository-relative src/**/*.c path: {path!r}"
        )


def load_manifest(repository: Path) -> tuple[str, ...]:
    """Parse the manifest and validate its local syntax and file paths."""

    repository = repository.resolve()
    manifest = repository / MANIFEST_PATH
    try:
        text = manifest.read_text(encoding="utf-8")
    except FileNotFoundError as error:
        raise ManifestError(
            f"native source manifest does not exist: {manifest}"
        ) from error

    paths: list[str] = []
    seen: dict[str, int] = {}
    started = False
    finished = False
    for line_number, raw in enumerate(text.splitlines(), 1):
        row = raw.strip()
        if not row or row.startswith("#"):
            continue
        if not started:
            if row != _MANIFEST_START:
                raise ManifestError(
                    f"line {line_number}: expected {_MANIFEST_START!r}"
                )
            started = True
            continue
        if finished:
            raise ManifestError(
                f"line {line_number}: unexpected content after source list"
            )
        if row == ")":
            finished = True
            continue
        path = row
        _validate_source_path(path, line_number=line_number)
        if path in seen:
            raise ManifestError(
                f"line {line_number}: duplicate native source {path!r}; "
                f"first declared on line {seen[path]}"
            )
        seen[path] = line_number
        if not (repository / path).is_file():
            raise ManifestError(
                f"line {line_number}: listed native source does not exist: {path}"
            )
        paths.append(path)

    if not started or not finished:
        raise ManifestError(
            "native source manifest has an incomplete CMake set() block"
        )
    if not paths:
        raise ManifestError("native source manifest contains no entries")
    if paths != sorted(paths):
        first = next(
            index for index, (actual, expected) in enumerate(zip(paths, sorted(paths)))
            if actual != expected
        )
        raise ManifestError(
            "native source manifest must be sorted by source path; "
            f"entry {first + 1} is {paths[first]!r}"
        )
    return tuple(paths)


def load_reviewed_components(repository: Path) -> dict[str, tuple[str, ...]]:
    """Load and validate the reviewed component path declarations."""

    configuration_path = repository / PROJECT_CONFIGURATION_PATH
    try:
        configuration = json.loads(configuration_path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ManifestError(
            f"project configuration does not exist: {configuration_path}"
        ) from error
    except json.JSONDecodeError as error:
        raise ManifestError(f"invalid {PROJECT_CONFIGURATION_PATH}: {error}") from error

    raw_components = configuration.get("components")
    if not isinstance(raw_components, list):
        raise ManifestError("archbird.json components must be an array")
    components: dict[str, tuple[str, ...]] = {}
    for index, raw_component in enumerate(raw_components):
        if not isinstance(raw_component, dict):
            raise ManifestError(f"archbird.json component {index} must be an object")
        name = raw_component.get("name")
        paths = raw_component.get("paths")
        if not isinstance(name, str) or not _COMPONENT_RE.fullmatch(name):
            raise ManifestError(
                f"archbird.json component {index} has an invalid name"
            )
        if name in components:
            raise ManifestError(f"archbird.json has duplicate component {name!r}")
        if (
            not isinstance(paths, list)
            or not paths
            or any(not isinstance(path, str) or not path for path in paths)
        ):
            raise ManifestError(
                f"archbird.json component {name!r} must have non-empty string paths"
            )
        components[name] = tuple(paths)
    return components


def reviewed_component_owners(
    path: str, components: dict[str, tuple[str, ...]]
) -> tuple[str, ...]:
    """Return every reviewed component whose path patterns own ``path``."""

    return tuple(
        sorted(
            name
            for name, patterns in components.items()
            if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)
        )
    )


def require_reviewed_component_owner(
    path: str, components: dict[str, tuple[str, ...]]
) -> str:
    """Return the one reviewed owner for ``path`` or reject configuration drift."""

    owners = reviewed_component_owners(path, components)
    if len(owners) != 1:
        rendered = ", ".join(owners) if owners else "none"
        raise ManifestError(
            f"{path}: expected exactly one reviewed archbird.json "
            f"component, found {rendered}"
        )
    return owners[0]


def validate_manifest(repository: Path) -> tuple[SourceEntry, ...]:
    """Prove source completeness and agreement with reviewed components."""

    repository = repository.resolve()
    paths = load_manifest(repository)
    declared = set(paths)
    actual = {
        path.relative_to(repository).as_posix()
        for path in (repository / "src").rglob("*.c")
        if path.is_file()
    }
    unlisted = sorted(actual - declared)
    if unlisted:
        raise ManifestError("unlisted native source(s): " + ", ".join(unlisted))
    stale = sorted(declared - actual)
    if stale:
        raise ManifestError("stale native source declaration(s): " + ", ".join(stale))

    components = load_reviewed_components(repository)
    entries: list[SourceEntry] = []
    for path in paths:
        entries.append(
            SourceEntry(require_reviewed_component_owner(path, components), path)
        )
    return tuple(entries)


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "repository",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument(
        "--print-paths",
        action="store_true",
        help="print the validated repository-relative source paths",
    )
    options = parser.parse_args(arguments)
    try:
        entries = validate_manifest(options.repository)
    except ManifestError as error:
        print(f"native core source manifest failed: {error}", file=sys.stderr)
        return 1
    if options.print_paths:
        print(" ".join(entry.path for entry in entries))
    else:
        component_count = len({entry.component for entry in entries})
        print(
            f"native core source manifest passed: {len(entries)} files, "
            f"{component_count} components"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
