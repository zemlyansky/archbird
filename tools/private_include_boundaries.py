#!/usr/bin/env python3
"""Validate module-qualified private includes and reviewed component edges."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path, PurePosixPath
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.core_source_manifest import (
    ManifestError,
    PROJECT_CONFIGURATION_PATH,
    load_reviewed_components,
    require_reviewed_component_owner,
)


_INCLUDE_DIRECTIVE_RE = re.compile(r"^\s*#\s*include\b(.*)$")
_LITERAL_INCLUDE_RE = re.compile(r'^\s*([<"])([^">]+)[">]')
_NATIVE_ROOTS = ("src", "bindings", "test")


class IncludeBoundaryError(ValueError):
    """Private header naming or component dependency policy was violated."""


@dataclass(frozen=True)
class IncludeRelation:
    source: str
    source_component: str
    target: str
    target_component: str
    line: int


@dataclass(frozen=True)
class IncludeInventory:
    header_count: int
    relations: tuple[IncludeRelation, ...]

    @property
    def component_edges(self) -> tuple[tuple[str, str], ...]:
        return tuple(
            sorted(
                {
                    (relation.source_component, relation.target_component)
                    for relation in self.relations
                    if relation.source_component != relation.target_component
                }
            )
        )


def _load_allowed_component_edges(
    repository: Path, component_names: set[str]
) -> set[tuple[str, str]]:
    configuration_path = repository / PROJECT_CONFIGURATION_PATH
    try:
        configuration = json.loads(configuration_path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise IncludeBoundaryError(
            f"project configuration does not exist: {configuration_path}"
        ) from error
    except json.JSONDecodeError as error:
        raise IncludeBoundaryError(
            f"invalid {PROJECT_CONFIGURATION_PATH}: {error}"
        ) from error

    try:
        constraint = configuration["constraints"]["NATIVE-COMPONENT-EDGES"]
        rows = constraint["expected"]["literal"]
    except (KeyError, TypeError) as error:
        raise IncludeBoundaryError(
            "archbird.json must declare constraints.NATIVE-COMPONENT-EDGES."
            "expected.literal"
        ) from error
    if not isinstance(rows, list):
        raise IncludeBoundaryError(
            "NATIVE-COMPONENT-EDGES expected literal must be an array"
        )

    edges: set[tuple[str, str]] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise IncludeBoundaryError(
                f"NATIVE-COMPONENT-EDGES row {index} must be an object"
            )
        source = row.get("source")
        target = row.get("target")
        kind = row.get("kind")
        if (
            not isinstance(source, str)
            or not source
            or not isinstance(target, str)
            or not target
            or kind != "*"
        ):
            raise IncludeBoundaryError(
                f"NATIVE-COMPONENT-EDGES row {index} must contain non-empty "
                "source/target and wildcard kind"
            )
        unknown = sorted({source, target} - component_names)
        if unknown:
            raise IncludeBoundaryError(
                f"NATIVE-COMPONENT-EDGES row {index} references unknown "
                f"component(s): {', '.join(unknown)}"
            )
        edge = (source, target)
        if edge in edges:
            raise IncludeBoundaryError(
                f"NATIVE-COMPONENT-EDGES contains duplicate {source}->{target}"
            )
        edges.add(edge)
    return edges


def _native_paths(repository: Path) -> tuple[Path, ...]:
    paths: list[Path] = []
    for root_name in _NATIVE_ROOTS:
        root = repository / root_name
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in {".c", ".h"}:
                continue
            relative = path.relative_to(repository)
            if relative.parts[:2] == ("test", "fixtures"):
                continue
            paths.append(path)
    return tuple(sorted(paths))


def validate_private_includes(repository: Path) -> IncludeInventory:
    """Inventory private headers and reject naming or dependency escapes."""

    repository = repository.resolve()
    try:
        components = load_reviewed_components(repository)
    except ManifestError as error:
        raise IncludeBoundaryError(str(error)) from error
    allowed_edges = _load_allowed_component_edges(repository, set(components))
    private_headers = {
        path.relative_to(repository / "src").as_posix(): path
        for path in (repository / "src").rglob("*.h")
        if path.is_file()
    }
    if not private_headers:
        raise IncludeBoundaryError("no private src/**/*.h headers found")

    header_owners: dict[str, str] = {}
    for include, path in sorted(private_headers.items()):
        relative = path.relative_to(repository).as_posix()
        try:
            header_owners[include] = require_reviewed_component_owner(
                relative, components
            )
        except ManifestError as error:
            raise IncludeBoundaryError(str(error)) from error

    relations: list[IncludeRelation] = []
    failures: list[str] = []
    for path in _native_paths(repository):
        source = path.relative_to(repository).as_posix()
        try:
            source_component = require_reviewed_component_owner(source, components)
        except ManifestError as error:
            raise IncludeBoundaryError(str(error)) from error
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), 1
        ):
            directive = _INCLUDE_DIRECTIVE_RE.match(line)
            if directive is None:
                continue
            match = _LITERAL_INCLUDE_RE.match(directive.group(1))
            if match is None:
                failures.append(
                    f"{source}:{line_number}: include directives must use "
                    "literal header paths"
                )
                continue
            delimiter = match.group(1)
            include = match.group(2)
            candidate = PurePosixPath(include)
            if candidate.is_absolute() or ".." in candidate.parts:
                failures.append(
                    f"{source}:{line_number}: native include escape is forbidden: "
                    f"{include!r}"
                )
                continue
            if include.startswith("./") or "\\" in include:
                failures.append(
                    f"{source}:{line_number}: include path must be canonical: "
                    f"{include!r}"
                )
                continue
            if include in private_headers:
                target = f"src/{include}"
                target_component = header_owners[include]
                relation = IncludeRelation(
                    source,
                    source_component,
                    target,
                    target_component,
                    line_number,
                )
                relations.append(relation)
                if delimiter != '"':
                    failures.append(
                        f"{source}:{line_number}: private include must use quotes: "
                        f"{include!r}"
                    )
                edge = (source_component, target_component)
                if source_component != target_component and edge not in allowed_edges:
                    failures.append(
                        f"{source}:{line_number}: forbidden private include edge "
                        f"{source_component}->{target_component}: {include!r}"
                    )
                continue

            suffix = f"/{include}"
            matches = sorted(
                f"src/{private}"
                for private in private_headers
                if f"src/{private}".endswith(suffix)
            )
            if matches:
                failures.append(
                    f"{source}:{line_number}: private include must be "
                    f"module-qualified from src/: {include!r}; "
                    f"candidate(s): {', '.join(matches)}"
                )
            elif include.startswith("src/"):
                failures.append(
                    f"{source}:{line_number}: private include must omit the src/ "
                    f"prefix: {include!r}"
                )

    if failures:
        raise IncludeBoundaryError("\n".join(failures))
    return IncludeInventory(len(private_headers), tuple(relations))


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
        inventory = validate_private_includes(options.repository)
    except IncludeBoundaryError as error:
        print(f"private include boundaries failed: {error}", file=sys.stderr)
        return 1
    print(
        "private include boundaries passed: "
        f"{inventory.header_count} headers, {len(inventory.relations)} relations, "
        f"{len(inventory.component_edges)} cross-component edges"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
