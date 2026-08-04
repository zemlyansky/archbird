#!/usr/bin/env python3
"""Compare Python wrapper requirements with both native adapter surfaces."""

from __future__ import annotations

import argparse
import ast
import importlib.machinery
import json
from pathlib import Path
import sys
from typing import Iterable


def _public_functions(path: Path) -> set[str]:
    tree = ast.parse(path.read_bytes(), filename=str(path))
    return {
        node.name
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and not node.name.startswith("_")
    }


def _native_references(paths: Iterable[Path]) -> tuple[set[str], set[str]]:
    references: set[str] = set()
    calls: set[str] = set()
    for path in paths:
        tree = ast.parse(path.read_bytes(), filename=str(path))
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Attribute)
                and isinstance(node.value, ast.Name)
                and node.value.id == "_native"
            ):
                references.add(node.attr)
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "_native"
            ):
                calls.add(node.func.attr)
    return references, calls


def _extension_operations(module: object) -> set[str]:
    return {
        name
        for name in dir(module)
        if not name.startswith("_")
        and name != "Error"
        and callable(getattr(module, name))
    }


def _is_extension(path: Path) -> bool:
    value = str(path)
    return any(value.endswith(suffix) for suffix in importlib.machinery.EXTENSION_SUFFIXES)


def _inside(child: Path, parent: Path) -> bool:
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def contract_failures(
    *,
    wrapper_references: set[str],
    wrapper_calls: set[str],
    ctypes_operations: set[str],
    compiled_operations: set[str],
    compiled_attributes: set[str],
) -> dict[str, list[str]]:
    failures = {
        "wrapper calls absent from ctypes adapter": sorted(
            wrapper_calls - ctypes_operations
        ),
        "wrapper calls absent from compiled extension": sorted(
            wrapper_calls - compiled_operations
        ),
        "ctypes operations absent from compiled extension": sorted(
            ctypes_operations - compiled_operations
        ),
        "compiled operations absent from ctypes adapter": sorted(
            compiled_operations - ctypes_operations
        ),
        "wrapper attributes absent from compiled extension": sorted(
            wrapper_references - compiled_attributes
        ),
    }
    return {label: values for label, values in failures.items() if values}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output")
    args = parser.parse_args()

    import archbird
    import archbird._native as compiled

    package = Path(archbird.__file__).resolve().parent
    compiled_path = Path(compiled.__file__).resolve()
    if not _inside(package, Path(sys.prefix).resolve()):
        raise AssertionError(
            f"archbird was not imported from the isolated installation: {package}"
        )
    if not _inside(compiled_path, package) or not _is_extension(compiled_path):
        raise AssertionError(
            f"archbird._native is not the installed compiled extension: {compiled_path}"
        )

    adapter_path = package / "_native.py"
    if not adapter_path.is_file():
        raise AssertionError("installed package omitted the ctypes adapter contract")
    wrapper_paths = tuple(
        path
        for path in sorted(package.rglob("*.py"))
        if path != adapter_path and "__pycache__" not in path.parts
    )
    wrapper_references, wrapper_calls = _native_references(wrapper_paths)
    ctypes_operations = _public_functions(adapter_path)
    compiled_operations = _extension_operations(compiled)

    missing_ctypes_calls = sorted(wrapper_calls - ctypes_operations)
    missing_compiled_calls = sorted(wrapper_calls - compiled_operations)
    missing_compiled_operations = sorted(ctypes_operations - compiled_operations)
    extra_compiled_operations = sorted(compiled_operations - ctypes_operations)
    missing_compiled_references = sorted(
        name for name in wrapper_references if not hasattr(compiled, name)
    )

    report = {
        "artifact": "archbird-python-native-contract",
        "compiled_extension": str(compiled_path),
        "compiled_operations": sorted(compiled_operations),
        "ctypes_operations": sorted(ctypes_operations),
        "extra_compiled_operations": extra_compiled_operations,
        "missing_compiled_calls": missing_compiled_calls,
        "missing_compiled_operations": missing_compiled_operations,
        "missing_compiled_references": missing_compiled_references,
        "missing_ctypes_calls": missing_ctypes_calls,
        "implementation_sha256": compiled.IMPLEMENTATION_SHA256,
        "version": archbird.__version__,
        "wrapper_calls": sorted(wrapper_calls),
        "wrapper_references": sorted(wrapper_references),
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(encoded, encoding="utf-8")

    active = contract_failures(
        wrapper_references=wrapper_references,
        wrapper_calls=wrapper_calls,
        ctypes_operations=ctypes_operations,
        compiled_operations=compiled_operations,
        compiled_attributes=set(dir(compiled)),
    )
    if active:
        raise AssertionError(
            "Python native adapter contract differs:\n"
            + json.dumps(active, indent=2, sort_keys=True)
        )
    print(
        "python native contract passed: "
        f"operations={len(compiled_operations)} "
        f"wrapper-calls={len(wrapper_calls)} "
        f"wrapper-references={len(wrapper_references)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
