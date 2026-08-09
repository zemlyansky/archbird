#!/usr/bin/env python3
"""Validate coarse native targets against sources and private includes."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.core_source_manifest import (
    ManifestError,
    load_reviewed_components,
    require_reviewed_component_owner,
    validate_manifest,
)
from tools.private_include_boundaries import (
    IncludeBoundaryError,
    validate_private_includes,
)


TARGET_MANIFEST_PATH = Path("cmake/ArchbirdCoreTargets.cmake")
_GROUPS_VARIABLE = "ARCHBIRD_DECLARED_CORE_GROUPS"
_TREE_SITTER_GROUP_VARIABLE = "ARCHBIRD_CORE_TREE_SITTER_GROUP"
_GROUP_RE = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")
_PREFIX_RE = re.compile(r"[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+)*\Z")
_SET_START_RE = re.compile(r"set\(([A-Za-z0-9_]+)\Z")


class TargetManifestError(ValueError):
    """The coarse native target manifest is malformed or stale."""


@dataclass(frozen=True)
class CoreTargetGroup:
    name: str
    source_prefixes: tuple[str, ...]
    dependencies: tuple[str, ...]


@dataclass(frozen=True)
class CoreTargetManifest:
    groups: tuple[CoreTargetGroup, ...]
    tree_sitter_group: str


@dataclass(frozen=True)
class CoreTargetInventory:
    groups: tuple[CoreTargetGroup, ...]
    tree_sitter_group: str
    source_groups: tuple[tuple[str, str], ...]
    header_groups: tuple[tuple[str, str], ...]
    dependency_edges: tuple[tuple[str, str], ...]


def _parse_sets(path: Path) -> dict[str, tuple[str, ...]]:
    try:
        rows = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError as error:
        raise TargetManifestError(
            f"native core target manifest does not exist: {path}"
        ) from error

    result: dict[str, tuple[str, ...]] = {}
    active_name: str | None = None
    active_values: list[str] = []
    for line_number, raw in enumerate(rows, 1):
        row = raw.strip()
        if not row or row.startswith("#"):
            continue
        if active_name is None:
            match = _SET_START_RE.fullmatch(row)
            if match is None:
                raise TargetManifestError(
                    f"line {line_number}: expected a multiline CMake set()"
                )
            active_name = match.group(1)
            if active_name in result:
                raise TargetManifestError(
                    f"line {line_number}: duplicate variable {active_name}"
                )
            active_values = []
            continue
        if row == ")":
            result[active_name] = tuple(active_values)
            active_name = None
            active_values = []
            continue
        if any(character.isspace() for character in row):
            raise TargetManifestError(
                f"line {line_number}: target manifest values must be one token"
            )
        active_values.append(row)
    if active_name is not None:
        raise TargetManifestError(f"unterminated CMake set() for {active_name}")
    return result


def load_target_manifest(repository: Path) -> CoreTargetManifest:
    """Parse and locally validate the coarse target declarations."""

    sets = _parse_sets(repository.resolve() / TARGET_MANIFEST_PATH)
    try:
        names = sets.pop(_GROUPS_VARIABLE)
    except KeyError as error:
        raise TargetManifestError(
            f"target manifest must declare {_GROUPS_VARIABLE}"
        ) from error
    if not names:
        raise TargetManifestError("native core target manifest contains no groups")
    if len(names) != len(set(names)):
        raise TargetManifestError("native core target manifest has duplicate groups")
    try:
        tree_sitter_groups = sets.pop(_TREE_SITTER_GROUP_VARIABLE)
    except KeyError as error:
        raise TargetManifestError(
            f"target manifest must declare {_TREE_SITTER_GROUP_VARIABLE}"
        ) from error
    if len(tree_sitter_groups) != 1:
        raise TargetManifestError(
            f"{_TREE_SITTER_GROUP_VARIABLE} must name exactly one group"
        )
    tree_sitter_group = tree_sitter_groups[0]
    if tree_sitter_group not in names:
        raise TargetManifestError(
            f"Tree-sitter target references unknown group {tree_sitter_group}"
        )

    groups: list[CoreTargetGroup] = []
    processed: set[str] = set()
    all_prefixes: dict[str, str] = {}
    for name in names:
        if not _GROUP_RE.fullmatch(name):
            raise TargetManifestError(f"invalid native core group name: {name!r}")
        prefix_variable = f"ARCHBIRD_CORE_GROUP_{name}_SOURCE_PREFIXES"
        dependency_variable = f"ARCHBIRD_CORE_GROUP_{name}_DEPENDENCIES"
        try:
            prefixes = sets.pop(prefix_variable)
            dependencies = sets.pop(dependency_variable)
        except KeyError as error:
            raise TargetManifestError(
                f"native core group {name} is missing {error.args[0]}"
            ) from error
        if not prefixes:
            raise TargetManifestError(
                f"native core group {name} contains no source prefixes"
            )
        if prefixes != tuple(sorted(prefixes)):
            raise TargetManifestError(
                f"native core group {name} source prefixes must be sorted"
            )
        if dependencies != tuple(sorted(dependencies)):
            raise TargetManifestError(
                f"native core group {name} dependencies must be sorted"
            )
        if len(prefixes) != len(set(prefixes)):
            raise TargetManifestError(
                f"native core group {name} has duplicate source prefixes"
            )
        if len(dependencies) != len(set(dependencies)):
            raise TargetManifestError(
                f"native core group {name} has duplicate dependencies"
            )
        for prefix in prefixes:
            candidate = PurePosixPath(prefix)
            if (
                not _PREFIX_RE.fullmatch(prefix)
                or prefix != candidate.as_posix()
                or candidate.is_absolute()
                or ".." in candidate.parts
            ):
                raise TargetManifestError(
                    f"native core group {name} has invalid source prefix {prefix!r}"
                )
            previous = all_prefixes.get(prefix)
            if previous is not None:
                raise TargetManifestError(
                    f"source prefix {prefix!r} is repeated by {previous} and {name}"
                )
            all_prefixes[prefix] = name
        for dependency in dependencies:
            if dependency not in processed:
                raise TargetManifestError(
                    f"native core group {name} depends on unknown or later "
                    f"group {dependency}"
                )
        groups.append(CoreTargetGroup(name, prefixes, dependencies))
        processed.add(name)
    if sets:
        raise TargetManifestError(
            "unexpected target manifest variable(s): " + ", ".join(sorted(sets))
        )
    return CoreTargetManifest(tuple(groups), tree_sitter_group)


def _matching_group(path: str, groups: tuple[CoreTargetGroup, ...]) -> str:
    if not path.startswith("src/"):
        raise TargetManifestError(f"native target input is outside src/: {path}")
    relative = path.removeprefix("src/")
    matches = [
        group.name
        for group in groups
        for prefix in group.source_prefixes
        if relative.startswith(f"{prefix}/")
    ]
    if len(matches) != 1:
        rendered = ", ".join(matches) if matches else "none"
        raise TargetManifestError(
            f"{path}: expected exactly one coarse native target, found {rendered}"
        )
    return matches[0]


def validate_target_manifest(repository: Path) -> CoreTargetInventory:
    """Prove source/header partition and exact collapsed include edges."""

    repository = repository.resolve()
    manifest = load_target_manifest(repository)
    groups = manifest.groups
    try:
        source_entries = validate_manifest(repository)
        include_inventory = validate_private_includes(repository)
        components = load_reviewed_components(repository)
    except (ManifestError, IncludeBoundaryError) as error:
        raise TargetManifestError(str(error)) from error

    source_groups = tuple(
        (entry.path, _matching_group(entry.path, groups)) for entry in source_entries
    )
    header_paths = tuple(
        sorted(
            path.relative_to(repository).as_posix()
            for path in (repository / "src").rglob("*.h")
            if path.is_file()
        )
    )
    header_groups = tuple(
        (path, _matching_group(path, groups)) for path in header_paths
    )

    component_groups: dict[str, str] = {}
    for path, group in (*source_groups, *header_groups):
        try:
            component = require_reviewed_component_owner(path, components)
        except ManifestError as error:
            raise TargetManifestError(str(error)) from error
        previous = component_groups.setdefault(component, group)
        if previous != group:
            raise TargetManifestError(
                f"reviewed component {component} is split across coarse targets "
                f"{previous} and {group}"
            )

    core_group_by_path = dict((*source_groups, *header_groups))
    actual_edges = {
        (core_group_by_path[relation.source], core_group_by_path[relation.target])
        for relation in include_inventory.relations
        if relation.source in core_group_by_path
        and relation.target in core_group_by_path
        and core_group_by_path[relation.source]
        != core_group_by_path[relation.target]
    }
    declared_edges = {
        (group.name, dependency)
        for group in groups
        for dependency in group.dependencies
    }
    missing = sorted(actual_edges - declared_edges)
    unexpected = sorted(declared_edges - actual_edges)
    if missing or unexpected:
        def render_edges(edges: list[tuple[str, str]]) -> str:
            return (
                ", ".join(f"{source}->{target}" for source, target in edges)
                or "none"
            )

        raise TargetManifestError(
            "coarse native target dependency drift; "
            f"missing={render_edges(missing)}, "
            f"unexpected={render_edges(unexpected)}"
        )

    return CoreTargetInventory(
        groups,
        manifest.tree_sitter_group,
        source_groups,
        header_groups,
        tuple(sorted(actual_edges)),
    )


def main(arguments: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "repository",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    options = parser.parse_args(arguments)
    try:
        inventory = validate_target_manifest(options.repository)
    except TargetManifestError as error:
        print(f"native core target manifest failed: {error}", file=sys.stderr)
        return 1
    print(
        "native core target manifest passed: "
        f"{len(inventory.groups)} groups, "
        f"{len(inventory.source_groups)} sources, "
        f"{len(inventory.header_groups)} headers, "
        f"{len(inventory.dependency_edges)} dependencies"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
