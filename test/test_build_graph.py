#!/usr/bin/env python3
"""Verify clean-test prerequisites and generated CTest library binding."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


PACK_DIRECTORIES = {
    "C": "c",
    "CPP": "cpp",
    "PYTHON": "python",
    "JAVASCRIPT": "javascript",
    "TYPESCRIPT": "typescript",
    "TSX": "tsx",
    "R": "r",
}
PACK_VENDOR_SOURCES = {
    "C": {"vendor/tree-sitter-c/src/parser.c"},
    "CPP": {
        "vendor/tree-sitter-cpp/src/parser.c",
        "vendor/tree-sitter-cpp/src/scanner.c",
    },
    "PYTHON": {
        "vendor/tree-sitter-python/src/parser.c",
        "vendor/tree-sitter-python/src/scanner.c",
    },
    "JAVASCRIPT": {
        "vendor/tree-sitter-javascript/src/parser.c",
        "vendor/tree-sitter-javascript/src/scanner.c",
    },
    "TYPESCRIPT": {
        "vendor/tree-sitter-typescript/typescript/src/parser.c",
        "vendor/tree-sitter-typescript/typescript/src/scanner.c",
    },
    "TSX": {
        "vendor/tree-sitter-typescript/tsx/src/parser.c",
        "vendor/tree-sitter-typescript/tsx/src/scanner.c",
    },
    "R": {
        "vendor/tree-sitter-r/src/parser.c",
        "vendor/tree-sitter-r/src/scanner.c",
    },
}


def logical_lines(text: str) -> list[str]:
    rows: list[str] = []
    pending = ""
    for raw in text.splitlines():
        row = pending + raw.lstrip() if pending else raw
        if row.endswith("\\"):
            pending = row[:-1] + " "
            continue
        rows.append(row)
        pending = ""
    if pending:
        rows.append(pending)
    return rows


def prerequisites(makefile: Path, target: str) -> set[str]:
    found = False
    result: set[str] = set()
    for row in logical_lines(makefile.read_text(encoding="utf-8")):
        if row.startswith(f"{target}:"):
            found = True
            result.update(row.split(":", 1)[1].split())
    if not found:
        raise AssertionError(f"Makefile has no {target!r} target")
    return result


def run(*arguments: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode:
        raise AssertionError(
            f"{' '.join(arguments)} exited {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def compiled_native_sources(repository: Path, build: Path) -> set[str]:
    commands = json.loads((build / "compile_commands.json").read_text())
    result: set[str] = set()
    for command in commands:
        path = Path(command["file"]).resolve()
        try:
            relative = path.relative_to(repository).as_posix()
        except ValueError:
            continue
        if relative.startswith("src/") or relative.startswith("vendor/tree-sitter"):
            result.add(relative)
    return result


def selected_core_sources(paths: set[str], enabled: set[str]) -> set[str]:
    syntax_root = "src/evidence/syntax/tree_sitter/"
    if not enabled:
        return {path for path in paths if not path.startswith(syntax_root)}
    result = set(paths)
    for pack, directory in PACK_DIRECTORIES.items():
        if pack in enabled or (pack == "TYPESCRIPT" and "TSX" in enabled):
            continue
        prefix = f"{syntax_root}{directory}/"
        result = {path for path in result if not path.startswith(prefix)}
    if not enabled.intersection({"JAVASCRIPT", "TYPESCRIPT", "TSX"}):
        prefix = f"{syntax_root}ecmascript/"
        result = {path for path in result if not path.startswith(prefix)}
    return result


def assert_source_matrix(
    repository: Path,
    build: Path,
    core_sources: set[str],
    enabled: set[str],
) -> None:
    expected = selected_core_sources(core_sources, enabled)
    if enabled:
        expected.add("vendor/tree-sitter/lib/src/lib.c")
        for pack in enabled:
            expected.update(PACK_VENDOR_SOURCES[pack])
    actual = compiled_native_sources(repository, build)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        raise AssertionError(
            f"native source selection drift in {build.name}; "
            f"missing={missing}, unexpected={unexpected}"
        )


def main() -> int:
    repository = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    sys.path.insert(0, str(repository))
    from tools.core_source_manifest import validate_manifest

    core_sources = {entry.path for entry in validate_manifest(repository)}
    required = prerequisites(repository / "Makefile", "test-js")
    if "native-wasm-smoke" not in required:
        raise AssertionError(
            "test-js consumes the native Wasm tree without declaring "
            "native-wasm-smoke"
        )

    build_root = repository / "build"
    build_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="build-graph-contract-", dir=build_root
    ) as raw:
        candidate = Path(raw)
        run(
            "cmake",
            "-S",
            str(repository),
            "-B",
            str(candidate),
            "-DBUILD_TESTING=ON",
            "-DARCHBIRD_BUILD_PYTHON=OFF",
            "-DARCHBIRD_BUILD_NODE=OFF",
            "-DARCHBIRD_BUILD_SHARED=ON",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            cwd=repository,
        )
        assert_source_matrix(
            repository, candidate, core_sources, set(PACK_DIRECTORIES)
        )
        inventory = json.loads(
            run(
                "ctest",
                "--test-dir",
                str(candidate),
                "--show-only=json-v1",
                cwd=repository,
            ).stdout
        )
        matches = [
            test
            for test in inventory["tests"]
            if test["name"] == "archbird_project_configuration_differential"
        ]
        if len(matches) != 1 or "command" not in matches[0]:
            raise AssertionError("CMake omitted the project differential test")
        command = matches[0]["command"]
        bindings = [
            value for value in command if value.startswith("ARCHBIRD_LIB=")
        ]
        if len(bindings) != 1:
            raise AssertionError(
                "project differential test does not bind exactly one built library"
            )
        library = Path(bindings[0].split("=", 1)[1])
        if candidate not in library.parents:
            raise AssertionError(
                f"project differential test escaped its CMake tree: {library}"
            )

        static_only = candidate / "static-only"
        run(
            "cmake",
            "-S",
            str(repository),
            "-B",
            str(static_only),
            "-DBUILD_TESTING=ON",
            "-DARCHBIRD_BUILD_PYTHON=OFF",
            "-DARCHBIRD_BUILD_NODE=OFF",
            "-DARCHBIRD_BUILD_SHARED=OFF",
            cwd=repository,
        )
        static_inventory = json.loads(
            run(
                "ctest",
                "--test-dir",
                str(static_only),
                "--show-only=json-v1",
                cwd=repository,
            ).stdout
        )
        if any(
            test["name"] == "archbird_project_configuration_differential"
            for test in static_inventory["tests"]
        ):
            raise AssertionError(
                "static-only CTest retained a Python differential test that "
                "requires a shared library"
            )

        matrix = {
            "syntax-off": (set(), ["-DARCHBIRD_ENABLE_TREE_SITTER=OFF"]),
            "c-only": (
                {"C"},
                [
                    f"-DARCHBIRD_ENABLE_TREE_SITTER_{pack}="
                    f"{'ON' if pack == 'C' else 'OFF'}"
                    for pack in PACK_DIRECTORIES
                ],
            ),
            "tsx-only": (
                {"TSX"},
                [
                    f"-DARCHBIRD_ENABLE_TREE_SITTER_{pack}="
                    f"{'ON' if pack == 'TSX' else 'OFF'}"
                    for pack in PACK_DIRECTORIES
                ],
            ),
        }
        for name, (enabled, options) in matrix.items():
            selection_build = candidate / f"source-matrix-{name}"
            run(
                "cmake",
                "-S",
                str(repository),
                "-B",
                str(selection_build),
                "-DBUILD_TESTING=OFF",
                "-DARCHBIRD_BUILD_PYTHON=OFF",
                "-DARCHBIRD_BUILD_NODE=OFF",
                "-DARCHBIRD_BUILD_SHARED=OFF",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                *options,
                cwd=repository,
            )
            assert_source_matrix(
                repository, selection_build, core_sources, enabled
            )
    print("clean test dependency graph contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
