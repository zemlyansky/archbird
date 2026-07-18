#!/usr/bin/env python3
"""Compare native map-config acceptance with a maintainer reference."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import sys

from archbird.map.config import ConfigError, load_config


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def native_accepts(extension, data: dict) -> bool:
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [],
        "producer": {
            "implementation_sha256": "7" * 64,
            "name": "config-reference-test",
            "version": "1",
        },
        "project": data.get("project", "invalid"),
        "schema_version": 1,
    }
    try:
        project = extension.project_create(canonical(manifest))
        extension.project_set_config(project, canonical(data))
    except (extension.Error, ValueError):
        return False
    return True


def python_accepts(data: dict, scratch: Path, root: Path) -> bool:
    scratch.write_bytes(canonical(data))
    try:
        load_config(scratch, root_override=root)
    except ConfigError:
        return False
    return True


def base_config() -> dict:
    return {
        "schema_version": 1,
        "project": "config-reference",
        "layers": [
            {
                "name": "core",
                "language": "c",
                "globs": ["src/**/*.c"],
            },
            {
                "name": "python",
                "language": "python",
                "globs": ["py/**/*.py"],
                "import_roots": ["py"],
            },
        ],
        "packages": [
            {
                "name": "python-package",
                "kind": "python",
                "path": "pyproject.toml",
                "layer": "python",
                "entries": ["archbird"],
                "identity": "archbird",
                "aliases": ["arch-bird"],
            }
        ],
        "builds": [{"name": "make", "kind": "make", "path": "Makefile"}],
        "indexes": [
            {
                "name": "semantic",
                "format": "scip",
                "path": "index.scip",
                "path_prefix": "src",
                "required": False,
            }
        ],
        "artifacts": [
            {
                "name": "native",
                "output": "build/libarchbird.a",
                "inputs": ["src/**"],
                "loaded_by": [
                    {"paths": ["Makefile"], "pattern": "libarchbird\\.a"}
                ],
                "builds": [{"source": "Makefile", "target": "native-build"}],
            }
        ],
        "bridges": [
            {
                "name": "python-abi",
                "kind": "abi",
                "from": "python",
                "to": ["core"],
                "prefixes": ["archbird_"],
                "providers": [
                    {
                        "kind": "file_pattern",
                        "path": "src/archbird.h",
                        "pattern": "(archbird_[A-Za-z0-9_]+)",
                    }
                ],
            }
        ],
        "tests": [
            {
                "name": "native-tests",
                "language": "c",
                "globs": ["tests/**/*.c"],
                "route_to": ["core"],
                "case_routes": [
                    {"selector": "json*", "to": "src/json.c"}
                ],
            }
        ],
        "components": [{"name": "core", "paths": ["src/**"]}],
        "named_entries": [
            {
                "name": "commands",
                "kind": "call_string_arg",
                "functions": ["run"],
                "globs": ["src/**"],
            }
        ],
        "parity": [
            {
                "name": "api",
                "members": [
                    {"label": "c", "source": "symbols", "layer": "core"},
                    {
                        "label": "py",
                        "source": "package",
                        "package": "python-package",
                    },
                ],
                "aliases": {"old": "new"},
            }
        ],
        "checks": {
            "symbols": [{"layer": "core", "name": "archbird_engine_create"}],
            "bridges": ["python-abi"],
            "entrypoints": [{"package": "python-package", "route": "."}],
            "test_routes": [{"group": "native-tests", "to": "src/json.c"}],
            "surfaces": [{"bridge": "python-abi"}],
        },
    }


def mutations() -> list[tuple[str, dict]]:
    base = base_config()
    cases: list[tuple[str, dict]] = [("rich-valid", base)]

    def add(name: str, change) -> None:
        item = copy.deepcopy(base)
        change(item)
        cases.append((name, item))

    add("unknown-top-field", lambda row: row.__setitem__("mystery", 1))
    add("whitespace-project", lambda row: row.__setitem__("project", "\u3000"))
    add("bad-package-kind", lambda row: row["packages"][0].__setitem__("kind", "x"))
    add("bad-package-layer", lambda row: row["packages"][0].__setitem__("layer", "x"))
    add("bad-build-kind", lambda row: row["builds"][0].__setitem__("kind", "x"))
    add("bad-index-format", lambda row: row["indexes"][0].__setitem__("format", "x"))
    add("parent-index-path", lambda row: row["indexes"][0].__setitem__("path", "../x"))
    add("noncanonical-index-prefix", lambda row: row["indexes"][0].__setitem__("path_prefix", "a//b"))
    add(
        "bad-index-position-fallback",
        lambda row: row["indexes"][0].__setitem__(
            "position_encoding_fallback", "bytes"
        ),
    )
    add("invalid-loader-regex", lambda row: row["artifacts"][0]["loaded_by"][0].__setitem__("pattern", "["))
    add("unknown-artifact-build", lambda row: row["artifacts"][0]["builds"][0].__setitem__("source", "Other"))
    add("bad-bridge-kind", lambda row: row["bridges"][0].__setitem__("kind", "x"))
    add("unknown-bridge-layer", lambda row: row["bridges"][0].__setitem__("to", "x"))
    add("provider-zero-captures", lambda row: row["bridges"][0]["providers"][0].__setitem__("pattern", "archbird_.*"))
    add("provider-two-captures", lambda row: row["bridges"][0]["providers"][0].__setitem__("pattern", "(archbird_)(.*)"))
    add("message-provider", lambda row: row["bridges"][0].__setitem__("kind", "message"))
    add("unknown-test-route", lambda row: row["tests"][0].__setitem__("route_to", ["x"]))
    add("bad-test-language", lambda row: row["tests"][0].__setitem__("language", "x"))
    add("bad-named-kind", lambda row: row["named_entries"][0].__setitem__("kind", "x"))
    add("short-parity", lambda row: row["parity"][0].__setitem__("members", row["parity"][0]["members"][:1]))
    add("duplicate-parity-label", lambda row: row["parity"][0]["members"][1].__setitem__("label", "c"))
    add("unknown-parity-package", lambda row: row["parity"][0]["members"][1].__setitem__("package", "x"))
    add("invalid-parity-regex-deferred", lambda row: row["parity"][0]["members"][0].__setitem__("include", ["["]))
    add("unknown-check-layer", lambda row: row["checks"]["symbols"][0].__setitem__("layer", "x"))
    add("unknown-check-bridge", lambda row: row["checks"].__setitem__("bridges", ["x"]))
    add("unknown-check-package", lambda row: row["checks"]["entrypoints"][0].__setitem__("package", "x"))
    add("unknown-check-test", lambda row: row["checks"]["test_routes"][0].__setitem__("group", "x"))
    add("duplicate-layers", lambda row: row["layers"].append(copy.deepcopy(row["layers"][0])))
    return cases


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_config_reference.py EXTENSION ROOT SCRATCH")
    extension = load_extension(Path(sys.argv[1]).resolve())
    root = Path(sys.argv[2]).resolve()
    scratch = Path(sys.argv[3]).resolve()
    scratch.parent.mkdir(parents=True, exist_ok=True)

    checked = 0
    for path in sorted((root / "examples").glob("*.json")):
        data = json.loads(path.read_text())
        if "layers" not in data:
            continue
        # The immutable pre-native oracle predates the separate semantic-index
        # byte budget. Compare its accepted projection while requiring the
        # native validator to accept the complete public example.
        oracle_data = copy.deepcopy(data)
        if isinstance(oracle_data.get("limits"), dict):
            oracle_data["limits"].pop("max_index_bytes", None)
        for test in oracle_data.get("tests", []):
            test.pop("case_extractors", None)
            test.pop("generated_files", None)
        assert python_accepts(oracle_data, scratch, root), path
        assert native_accepts(extension, data), path
        checked += 1

    for name, data in mutations():
        expected = python_accepts(data, scratch, root)
        actual = native_accepts(extension, data)
        assert actual == expected, (name, expected, actual)
        checked += 1
    scratch.unlink(missing_ok=True)
    print(f"native config parity passed: {checked} valid/mutated documents")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
