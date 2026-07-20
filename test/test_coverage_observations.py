#!/usr/bin/env python3
"""Generic coverage-report to observed test-route adapter regressions."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile

from archbird import validate_test_symbol_observations
from archbird.adapters.coverage import CoverageAdapterError, compile_test_observations


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def write(root: Path, name: str, value: bytes) -> dict[str, object]:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)
    return {"path": name, "sha256": digest(value), "symbols": []}


def request(format_name: str, report: str, test: str, runner: str, *, context=None):
    case = {"path": test, "report": "case", "selector": "suite.case"}
    if context is not None:
        case["context"] = context
    return {
        "artifact": "archbird-coverage-observation-request",
        "cases": [case],
        "format": format_name,
        "group": "suite",
        "reports": [{"id": "case", "path": report}],
        "runner_paths": [runner],
        "schema_version": 1,
    }


def compile(root: Path, map_value: object, request_value: object) -> dict[str, object]:
    encoded = compile_test_observations(
        canonical(map_value),
        canonical(request_value),
        request_directory=root,
        repository=root,
    )
    validate_test_symbol_observations(encoded)
    return json.loads(encoded)


def main() -> None:
    with tempfile.TemporaryDirectory(dir=Path.cwd() / "build" / "tmp") as raw:
        root = Path(raw)
        files = []
        files.append(write(root, "pyproject.toml", b"[tool.pytest.ini_options]\n"))
        files.append(
            write(root, "package.json", b'{"scripts":{"test":"node test.js"}}\n')
        )
        files.append(write(root, "Makefile", b"test:\n\t./test_core\n"))
        files.append(
            write(root, "tests/test_subject.py", b"def test_alpha():\n    pass\n")
        )
        files.append(write(root, "test/test.js", b"test('alpha', () => {});\n"))
        files.append(write(root, "test/test_core.c", b"int main(void) { return 0; }\n"))
        python_source = b"def alpha():\n    return 1\n\ndef beta():\n    return 2\n"
        python_file = write(root, "src/subject.py", python_source)
        python_file["symbols"] = [
            {"kind": "function", "line": 1, "name": "alpha", "public": True},
            {"kind": "function", "line": 4, "name": "beta", "public": True},
        ]
        files.append(python_file)
        javascript_source = (
            'const emoji = "😀";\nfunction alpha() { return 1; }\n'.encode()
        )
        javascript_file = write(root, "src/subject.js", javascript_source)
        javascript_file["symbols"] = [
            {"kind": "function", "line": 2, "name": "alpha", "public": True}
        ]
        files.append(javascript_file)
        c_source = b"int alpha(void) { return 1; }\n"
        c_file = write(root, "src/subject.c", c_source)
        c_file["symbols"] = [
            {"kind": "function", "line": 1, "name": "alpha", "public": True}
        ]
        files.append(c_file)
        files.sort(key=lambda row: str(row["path"]).encode())
        map_value = {
            "artifact": "map",
            "evidence": {"config_sha256": "1" * 64, "input_sha256": "2" * 64},
            "files": files,
            "project": "coverage-fixture",
        }

        (root / "coverage.json").write_text(
            json.dumps(
                {
                    "files": {
                        "src/subject.py": {
                            "contexts": {"2": ["tests/test_subject.py::test_alpha|run"]}
                        }
                    }
                }
            )
        )
        observed = compile(
            root,
            map_value,
            request(
                "coverage.py",
                "coverage.json",
                "tests/test_subject.py",
                "pyproject.toml",
                context="tests/test_subject.py::test_alpha|run",
            ),
        )
        assert observed["cases"][0]["symbols"] == [
            {"hits": 1, "path": "src/subject.py", "symbol": "alpha"}
        ]

        (root / "istanbul.json").write_text(
            json.dumps(
                {
                    str(root / "src/subject.js"): {
                        "fnMap": {
                            "0": {"name": "alpha", "decl": {"start": {"line": 2}}}
                        },
                        "f": {"0": 3},
                    }
                }
            )
        )
        observed = compile(
            root,
            map_value,
            request("istanbul", "istanbul.json", "test/test.js", "package.json"),
        )
        assert observed["cases"][0]["symbols"][0]["hits"] == 3

        (root / "llvm.json").write_text(
            json.dumps(
                {
                    "type": "llvm.coverage.json.export",
                    "data": [
                        {
                            "functions": [
                                {
                                    "count": 4,
                                    "filenames": [str(root / "src/subject.c")],
                                    "name": "alpha",
                                    "regions": [[1, 1, 1, 10, 4, 0, 0, 0]],
                                }
                            ]
                        }
                    ],
                }
            )
        )
        observed = compile(
            root,
            map_value,
            request("llvm", "llvm.json", "test/test_core.c", "Makefile"),
        )
        assert observed["cases"][0]["symbols"][0]["hits"] == 4

        (root / "gcov.json").write_text(
            json.dumps(
                {
                    "format_version": "2",
                    "files": [
                        {
                            "file": "src/subject.c",
                            "functions": [
                                {"execution_count": 5, "name": "alpha", "start_line": 1}
                            ],
                        }
                    ],
                }
            )
        )
        observed = compile(
            root,
            map_value,
            request("gcov", "gcov.json", "test/test_core.c", "Makefile"),
        )
        assert observed["cases"][0]["symbols"][0]["hits"] == 5

        bad = request(
            "coverage.py", "coverage.json", "tests/test_subject.py", "pyproject.toml"
        )
        try:
            compile(root, map_value, bad)
        except CoverageAdapterError as error:
            assert "dynamic context" in str(error)
        else:
            raise AssertionError("aggregate coverage.py input was accepted")

        bad = request("v8", "coverage.json", "test/test.js", "package.json")
        try:
            compile(root, map_value, bad)
        except CoverageAdapterError as error:
            assert "unsupported coverage format" in str(error)
        else:
            raise AssertionError("Python accepted V8 offsets without a JS runtime")

    print("coverage observation Python adapter tests passed")


if __name__ == "__main__":
    if len(sys.argv) != 1:
        raise SystemExit("usage: test_coverage_observations.py")
    main()
