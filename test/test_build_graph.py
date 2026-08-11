#!/usr/bin/env python3
"""Verify clean-test prerequisites and generated CTest library binding."""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from tools.core_target_manifest import CoreTargetInventory


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
SOAK_TESTS = {
    "archbird_native_allocator",
    "archbird_project_configuration_differential",
}
SCANNER_ABI_HEADER = Path("evidence/syntax/tree_sitter/scanner_abi_compat.h")


def assert_tree_sitter_scanner_abi_contract(repository: Path) -> None:
    scanner_sources = sorted(
        {
            source
            for sources in PACK_VENDOR_SOURCES.values()
            for source in sources
            if source.endswith("/scanner.c")
        }
    )
    definition = re.compile(
        r"\bvoid\s*\*\s*"
        r"(tree_sitter_[A-Za-z0-9_]+_external_scanner_create)"
        r"\s*\(([^)]*)\)\s*\{"
    )
    definitions: dict[str, tuple[str, str]] = {}
    for source in scanner_sources:
        matches = definition.findall(
            (repository / source).read_text(encoding="utf-8")
        )
        if len(matches) != 1:
            raise AssertionError(
                f"{source} has {len(matches)} scanner-create definitions; "
                "expected one"
            )
        symbol, parameters = matches[0]
        if parameters.strip() not in {"", "void"}:
            raise AssertionError(
                f"{source} has unexpected scanner-create parameters: "
                f"{parameters!r}"
            )
        if symbol in definitions:
            raise AssertionError(
                f"duplicate scanner-create definition for {symbol}: "
                f"{definitions[symbol][0]} and {source}"
            )
        definitions[symbol] = (source, parameters.strip())

    header = repository / "src" / SCANNER_ABI_HEADER
    prototypes = set(
        re.findall(
            r"\bvoid\s*\*\s*"
            r"(tree_sitter_[A-Za-z0-9_]+_external_scanner_create)"
            r"\s*\(void\)\s*;",
            header.read_text(encoding="utf-8"),
        )
    )
    symbols = set(definitions)
    if prototypes != symbols:
        raise AssertionError(
            "Tree-sitter scanner ABI prototype inventory drift; "
            f"missing={sorted(symbols - prototypes)}, "
            f"unexpected={sorted(prototypes - symbols)}"
        )


def cmake_module_text(repository: Path, filename: str) -> str:
    root_cmake = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
    include = f'include("${{CMAKE_CURRENT_SOURCE_DIR}}/cmake/{filename}")'
    if root_cmake.count(include) != 1:
        raise AssertionError(
            f"root CMake must include cmake/{filename} exactly once"
        )
    path = repository / "cmake" / filename
    if not path.is_file():
        raise AssertionError(f"root CMake includes missing module: {path}")
    return path.read_text(encoding="utf-8")


def assert_fuzz_sanitizer_contract(repository: Path) -> None:
    cmake = cmake_module_text(repository, "ArchbirdFuzzing.cmake")
    if "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1" not in cmake:
        raise AssertionError("fuzz smoke does not make UBSan findings fatal")
    if "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1" not in cmake:
        raise AssertionError("fuzz smoke does not make ASan findings fatal")
    smoke_options = (
        "-runs=256 -seed=1 -verbosity=0 -print_funcs=0 -max_len=65536"
    )
    if smoke_options not in cmake:
        raise AssertionError(
            "fuzz smoke must retain its deterministic run budget without "
            "starting a symbolizer for routine coverage updates"
        )
    unguarded = re.findall(r"\bCOMMAND\s+(archbird_fuzz_[A-Za-z0-9_]+)", cmake)
    if unguarded:
        raise AssertionError(
            "fuzz smoke commands bypass the fatal sanitizer environment: "
            f"{sorted(set(unguarded))}"
        )
    guarded = re.findall(
        r"\bCOMMAND\s+\$\{ARCHBIRD_FUZZ_RUN\}\s+"
        r"\$<TARGET_FILE:(archbird_fuzz_[A-Za-z0-9_]+)>",
        cmake,
    )
    if len(guarded) != 15 or len(set(guarded)) != 15:
        raise AssertionError(
            "fuzz smoke must run each of its 15 targets once through the "
            f"fatal sanitizer environment; observed={sorted(guarded)}"
        )


def assert_native_testing_module_contract(repository: Path) -> None:
    cmake = cmake_module_text(repository, "ArchbirdTesting.cmake")
    root_cmake = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
    helper = "function(archbird_add_native_test_program target)"
    if cmake.count(helper) != 1:
        raise AssertionError("native testing module must own one target helper")
    if "if(BUILD_TESTING)" not in cmake:
        raise AssertionError("native testing module is not guarded by BUILD_TESTING")
    required_sanitizer_fragments = {
        "function(archbird_apply_sanitizer_test_environment)",
        "get_property(ARCHBIRD_SANITIZER_TESTS DIRECTORY PROPERTY TESTS)",
        "set_tests_properties(${ARCHBIRD_SANITIZER_TESTS} PROPERTIES",
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1",
        "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1",
    }
    missing = sorted(
        fragment
        for fragment in required_sanitizer_fragments
        if fragment not in cmake
    )
    if missing:
        raise AssertionError(
            "sanitizer CTest diagnostics are not fatal for every registered "
            f"test: missing={missing}"
        )
    sanitizer_apply = "archbird_apply_sanitizer_test_environment()"
    if root_cmake.count(sanitizer_apply) != 1:
        raise AssertionError(
            "root CMake must apply the sanitizer environment exactly once "
            "after all optional frontend tests are registered"
        )
    last_frontend_test = max(
        root_cmake.index("NAME archbird_native_python_repository"),
        root_cmake.index("NAME archbird_native_node_frontend"),
        root_cmake.index("NAME archbird_native_resolution_frontend_parity"),
    )
    if root_cmake.index(sanitizer_apply) < last_frontend_test:
        raise AssertionError(
            "sanitizer environment is applied before optional frontend "
            "tests are registered"
        )


def assert_package_scanner_abi_contract(repository: Path) -> None:
    relative = SCANNER_ABI_HEADER.as_posix()
    setup = (repository / "py" / "setup.py").read_text(encoding="utf-8")
    expected_python_path = f"csrc/src/{relative}"
    required_python_fragments = {
        f'Path("{expected_python_path}")',
        'f"/FI{tree_sitter_scanner_abi_header}"',
        '"-include",\n        tree_sitter_scanner_abi_header,',
    }
    missing = sorted(
        fragment for fragment in required_python_fragments if fragment not in setup
    )
    if missing:
        raise AssertionError(
            f"Python source build omits scanner ABI contract fragments: {missing}"
        )

    binding = json.loads(
        (repository / "js" / "binding.gyp").read_text(encoding="utf-8")
    )
    targets = [
        target
        for target in binding.get("targets", [])
        if target.get("target_name") == "_native"
    ]
    if len(targets) != 1:
        raise AssertionError("binding.gyp must define exactly one _native target")
    platform_conditions = [
        condition
        for condition in targets[0]["conditions"]
        if condition[0].replace(" ", "") == "OS=='win'"
    ]
    expected_node_path = f"<(module_root_dir)/csrc/src/{relative}"
    if len(platform_conditions) != 1 or len(platform_conditions[0]) != 3:
        raise AssertionError("binding.gyp omits a platform scanner ABI route")
    _, windows, non_windows = platform_conditions[0]
    forced = windows["msvs_settings"]["VCCLCompilerTool"].get(
        "ForcedIncludeFiles"
    )
    if forced != [expected_node_path]:
        raise AssertionError(
            f"Windows Node build has wrong scanner ABI header: {forced}"
        )
    flags = non_windows.get("cflags_c", [])
    try:
        include_index = flags.index("-include")
    except ValueError as error:
        raise AssertionError(
            "non-Windows Node build omits forced scanner ABI include"
        ) from error
    if flags[include_index + 1 : include_index + 2] != [expected_node_path]:
        raise AssertionError(
            "non-Windows Node build has wrong scanner ABI header after -include"
        )


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


def recipe(makefile: Path, target: str) -> str:
    found = False
    active = False
    result: list[str] = []
    for row in logical_lines(makefile.read_text(encoding="utf-8")):
        if row.startswith(f"{target}:"):
            found = True
            active = True
            continue
        if not active:
            continue
        if row.startswith("\t"):
            result.append(row.strip())
        elif row.strip() and not row.lstrip().startswith("#"):
            active = False
    if not found:
        raise AssertionError(f"Makefile has no {target!r} target")
    if not result:
        raise AssertionError(f"Makefile target {target!r} has no recipe")
    return " ".join(result)


def assert_test_classes(
    inventory: dict[str, object], expected_soak: set[str]
) -> None:
    tests = inventory["tests"]
    if not isinstance(tests, list) or not tests:
        raise AssertionError("CMake registered no tests to classify")
    names = {test["name"] for test in tests}
    if not expected_soak <= names:
        raise AssertionError(
            f"CMake omitted soak tests: {sorted(expected_soak - names)}"
        )
    for test in tests:
        labels: set[str] = set()
        for property_ in test.get("properties", []):
            if property_["name"] != "LABELS":
                continue
            value = property_["value"]
            labels.update(value if isinstance(value, list) else [value])
        expected = {"soak"} if test["name"] in expected_soak else {"fast"}
        if labels != expected:
            raise AssertionError(
                f"CTest runtime class drift for {test['name']}: "
                f"observed={sorted(labels)}, expected={sorted(expected)}"
            )


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
        output = command.get("output", command.get("command", ""))
        if "archbird_core_" not in output:
            continue
        path = Path(command["file"]).resolve()
        try:
            relative = path.relative_to(repository).as_posix()
        except ValueError:
            continue
        if relative.startswith("src/") or relative.startswith("vendor/tree-sitter"):
            result.add(relative)
    return result


def assert_core_target_commands(
    repository: Path,
    build: Path,
    source_groups: dict[str, str],
    tree_sitter_group: str,
    enabled: set[str],
    *,
    shared: bool,
) -> None:
    commands = json.loads((build / "compile_commands.json").read_text())
    selected = selected_core_sources(set(source_groups), enabled)
    by_source: dict[str, list[dict[str, str]]] = {path: [] for path in selected}
    vendor_sources: set[str] = set()
    if enabled:
        vendor_sources.add("vendor/tree-sitter/lib/src/lib.c")
        for pack in enabled:
            vendor_sources.update(PACK_VENDOR_SOURCES[pack])
    by_vendor: dict[str, list[dict[str, str]]] = {
        path: [] for path in vendor_sources
    }
    for command in commands:
        output = command.get("output", command.get("command", ""))
        if "archbird_core_" not in output:
            continue
        path = Path(command["file"]).resolve()
        try:
            relative = path.relative_to(repository).as_posix()
        except ValueError:
            continue
        if relative in by_source:
            by_source[relative].append(command)
        elif relative in by_vendor:
            by_vendor[relative].append(command)

    variants = {"static", "shared"} if shared else {"static"}
    expected_count = len(variants)
    private_root = (build / "archbird-private-include").resolve()
    source_root = (repository / "src").resolve()
    for path, entries in (*by_source.items(), *by_vendor.items()):
        if len(entries) != expected_count:
            raise AssertionError(
                f"{path} compiled {len(entries)} times in {build.name}; "
                f"expected {expected_count}"
            )
        group = source_groups.get(path, tree_sitter_group)
        observed_variants: set[str] = set()
        for entry in entries:
            output = entry.get("output", entry.get("command", ""))
            matches = {
                variant
                for variant in variants
                if f"archbird_core_{variant}_{group}.dir" in output
            }
            if len(matches) != 1:
                raise AssertionError(
                    f"{path} escaped its coarse target {group} in {build.name}: "
                    f"{output}"
                )
            observed_variants.update(matches)

            arguments = entry.get("arguments")
            if arguments is None:
                arguments = shlex.split(entry["command"])
            if "-Wshadow" not in arguments and "/W4" not in arguments:
                raise AssertionError(
                    f"{path} has no shadow-warning ratchet in {build.name}"
                )
            if (
                "/W4" not in arguments
                and "-Wsign-conversion" not in arguments
            ):
                raise AssertionError(
                    f"{path} has no sign-conversion ratchet in {build.name}"
                )
            include_paths: list[Path] = []
            index = 0
            while index < len(arguments):
                argument = arguments[index]
                if argument == "-I" and index + 1 < len(arguments):
                    index += 1
                    include_paths.append(Path(arguments[index]).resolve())
                elif argument.startswith("-I") and len(argument) > 2:
                    include_paths.append(Path(argument[2:]).resolve())
                elif argument.startswith("/I") and len(argument) > 2:
                    include_paths.append(Path(argument[2:]).resolve())
                index += 1
            if source_root in include_paths:
                raise AssertionError(
                    f"{path} retains unrestricted private src visibility in "
                    f"{build.name}"
                )
            private_views = [
                include
                for include in include_paths
                if private_root in include.parents and include.name == group
            ]
            if len(private_views) != 1:
                raise AssertionError(
                    f"{path} does not have exactly one {group} private header "
                    f"view in {build.name}: {private_views}"
                )
            if path in vendor_sources:
                expected_header = (
                    private_views[0] / SCANNER_ABI_HEADER
                ).resolve()
                forced_headers: list[Path] = []
                index = 0
                while index < len(arguments):
                    argument = arguments[index]
                    if argument == "-include" and index + 1 < len(arguments):
                        index += 1
                        forced_headers.append(Path(arguments[index]).resolve())
                    elif argument.startswith("/FI") and len(argument) > 3:
                        forced_headers.append(Path(argument[3:]).resolve())
                    index += 1
                if forced_headers != [expected_header]:
                    raise AssertionError(
                        f"{path} does not receive exactly the reviewed scanner "
                        f"ABI header in {build.name}: {forced_headers}"
                    )
        if observed_variants != variants:
            raise AssertionError(
                f"{path} compiled for variants {sorted(observed_variants)} in "
                f"{build.name}; expected {sorted(variants)}"
            )


def assert_private_header_views(
    build: Path, target_inventory: CoreTargetInventory
) -> None:
    dependencies = {
        group.name: set(group.dependencies) for group in target_inventory.groups
    }
    header_groups = dict(target_inventory.header_groups)
    visible: dict[str, set[str]] = {}
    for group in target_inventory.groups:
        group_visible = {group.name}
        for dependency in dependencies[group.name]:
            group_visible.update(visible[dependency])
        visible[group.name] = group_visible

    private_root = build / "archbird-private-include"
    for group in target_inventory.groups:
        roots = list(private_root.glob(f"*/{group.name}"))
        if len(roots) != 1:
            raise AssertionError(
                f"{build.name} has {len(roots)} private header views for "
                f"{group.name}; expected one"
            )
        actual = {
            path.relative_to(roots[0]).as_posix()
            for path in roots[0].rglob("*.h")
            if path.is_file()
        }
        expected = {
            path.removeprefix("src/")
            for path, owner in header_groups.items()
            if owner in visible[group.name]
        }
        if actual != expected:
            raise AssertionError(
                f"private header view drift for {group.name} in {build.name}; "
                f"missing={sorted(expected - actual)}, "
                f"unexpected={sorted(actual - expected)}"
            )


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
    from tools.core_target_manifest import validate_target_manifest

    core_sources = {entry.path for entry in validate_manifest(repository)}
    target_inventory = validate_target_manifest(repository)
    source_groups = dict(target_inventory.source_groups)
    assert_tree_sitter_scanner_abi_contract(repository)
    assert_fuzz_sanitizer_contract(repository)
    assert_native_testing_module_contract(repository)
    assert_package_scanner_abi_contract(repository)
    makefile = repository / "Makefile"
    required = prerequisites(makefile, "test-js")
    if "native-wasm-smoke" not in required:
        raise AssertionError(
            "test-js consumes the native Wasm tree without declaring "
            "native-wasm-smoke"
        )
    for runtime_class in ("fast", "soak"):
        target = f"native-test-{runtime_class}"
        if "native-build" not in prerequisites(makefile, target):
            raise AssertionError(f"{target} does not depend on native-build")
        command = recipe(makefile, target)
        expected_filter = f"--label-regex '^{runtime_class}$$'"
        if expected_filter not in command or "--output-on-failure" not in command:
            raise AssertionError(
                f"{target} does not select only the {runtime_class} CTest class"
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
        assert_core_target_commands(
            repository,
            candidate,
            source_groups,
            target_inventory.tree_sitter_group,
            set(PACK_DIRECTORIES),
            shared=True,
        )
        assert_private_header_views(candidate, target_inventory)
        inventory = json.loads(
            run(
                "ctest",
                "--test-dir",
                str(candidate),
                "--show-only=json-v1",
                cwd=repository,
            ).stdout
        )
        assert_test_classes(inventory, SOAK_TESTS)
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
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            cwd=repository,
        )
        assert_core_target_commands(
            repository,
            static_only,
            source_groups,
            target_inventory.tree_sitter_group,
            set(PACK_DIRECTORIES),
            shared=False,
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
        assert_test_classes(
            static_inventory,
            SOAK_TESTS - {"archbird_project_configuration_differential"},
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
            assert_core_target_commands(
                repository,
                selection_build,
                source_groups,
                target_inventory.tree_sitter_group,
                enabled,
                shared=False,
            )
    print("clean test dependency graph contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
