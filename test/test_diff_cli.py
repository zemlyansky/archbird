#!/usr/bin/env python3
"""Cross-host structural diff risk and position-normalization contract."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from archbird import _native, cli as python_cli


def encode(document: object) -> bytes:
    return json.dumps(
        document,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def run_host(
    host: str,
    arguments: list[str],
    *,
    repository: Path,
    node: str,
    addon: Path,
) -> subprocess.CompletedProcess[bytes]:
    environment = os.environ.copy()
    if host == "python":
        environment["PYTHONPATH"] = str(repository / "py")
        command = [sys.executable, "-m", "archbird", *arguments]
    else:
        environment["ARCHBIRD_ENGINE"] = "native"
        environment["ARCHBIRD_NATIVE_ADDON"] = str(addon)
        command = [node, str(repository / "js/src/cli.js"), *arguments]
    return subprocess.run(
        command,
        cwd=repository,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def position_fixture(repository: Path) -> dict[str, object]:
    document = json.loads(
        (repository / "test/fixtures/report_map.json").read_text(
            encoding="utf-8"
        )
    )
    document.update(
        schema_version=9,
        facts=[],
        inputs=[],
        symbol_references=[],
    )
    document["symbol_calls"] = [
        {
            "candidates": [
                {"line": 1, "path": "src/target.c", "symbol": "target"}
            ],
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "call-1",
                    "line": 4,
                    "provider": "fixture",
                    "span": {"end": 20, "start": 14},
                }
            ],
            "name": "target",
            "resolution": "candidate",
            "source": {"path": "src/source.c", "symbol": "source"},
        }
    ]
    document["tests"][0]["cases"][0]["route_evidence"] = [
        {
            "claim": "syntax-reference",
            "enclosing": "",
            "fact_id": "route-1",
            "line": 4,
            "name": "target",
            "provenance": "derived",
            "provider": "fixture",
            "relation": "call",
            "scope": "case",
            "span": {"end": 24, "start": 18},
            "target": "src/target.c",
            "target_symbol": "target",
        }
    ]
    return document


def shifted_positions(before: dict[str, object]) -> dict[str, object]:
    after = copy.deepcopy(before)
    candidate = after["symbol_calls"][0]["candidates"][0]
    candidate["line"] = 9
    evidence = after["symbol_calls"][0]["evidence"][0]
    evidence.update(fact_id="call-shifted", line=5, span={"end": 30, "start": 24})
    route = after["tests"][0]["cases"][0]["route_evidence"][0]
    route.update(fact_id="route-shifted", line=5, span={"end": 34, "start": 28})
    return after


def write_case(root: Path, name: str, before: object, after: object) -> tuple[Path, Path]:
    before_path = root / f"{name}-before.json"
    after_path = root / f"{name}-after.json"
    before_path.write_bytes(encode(before))
    after_path.write_bytes(encode(after))
    return before_path, after_path


def assert_case(
    root: Path,
    name: str,
    before: object,
    after: object,
    category: str | None,
    expected: int,
    *,
    repository: Path,
    node: str,
    addon: Path,
) -> None:
    before_path, after_path = write_case(root, name, before, after)
    arguments = [
        "diff",
        "--before",
        str(before_path),
        "--after",
        str(after_path),
    ]
    arguments.append("--check" if category is None else f"--check={category}")
    results = {
        host: run_host(
            host,
            arguments,
            repository=repository,
            node=node,
            addon=addon,
        )
        for host in ("python", "node")
    }
    for host, result in results.items():
        if result.returncode != expected:
            raise AssertionError(
                f"{name}/{host} exited {result.returncode}, expected {expected}\n"
                f"stdout={result.stdout.decode(errors='replace')}\n"
                f"stderr={result.stderr.decode(errors='replace')}"
            )
    if expected != 2 and results["python"].stdout != results["node"].stdout:
        raise AssertionError(f"{name}: Python and Node diff bytes diverged")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_diff_cli.py REPOSITORY NODE ADDON")
    repository = Path(sys.argv[1]).resolve()
    node = sys.argv[2]
    addon = Path(sys.argv[3]).resolve()
    temporary_root = repository / "build/tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    before = position_fixture(repository)
    position_only = shifted_positions(before)
    position_delta = json.loads(_native.map_diff(encode(before), encode(position_only)))
    for section in ("symbol_calls", "symbol_references", "test_route_evidence"):
        if any(position_delta["sections"][section].values()):
            raise AssertionError({section: position_delta["sections"][section]})
    missing_section = copy.deepcopy(position_delta)
    del missing_section["sections"]["package_export_origins"]
    try:
        python_cli._diff_has_risk(missing_section, "public-api")
    except ValueError as error:
        if str(error) != "native diff result has no package_export_origins section":
            raise
    else:
        raise AssertionError("Python diff policy accepted a missing section")

    with tempfile.TemporaryDirectory(dir=temporary_root) as raw:
        root = Path(raw)
        assert_case(
            root,
            "position-only-calls",
            before,
            position_only,
            "calls",
            0,
            repository=repository,
            node=node,
            addon=addon,
        )

        comment_root = root / "comment-only-repository"
        module = comment_root / "pkg/m.py"
        module.parent.mkdir(parents=True)
        module.write_text(
            "def a():\n    return b()\n\n\ndef b():\n    return 1\n",
            encoding="utf-8",
        )
        generated_maps = []
        for label in ("before", "after"):
            if label == "after":
                module.write_text(
                    "# positional comment\n\n\n"
                    "def a():\n    return b()\n\n\ndef b():\n    return 1\n",
                    encoding="utf-8",
                )
            output = root / f"comment-{label}.json"
            result = run_host(
                "python",
                [
                    "map",
                    str(comment_root),
                    "--no-config",
                    "--no-cache",
                    "--progress",
                    "never",
                    "--format",
                    "json",
                    "--output",
                    str(output),
                    "--check",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            if result.returncode:
                raise AssertionError(
                    f"comment-only {label} Map failed: "
                    f"{result.stderr.decode(errors='replace')}"
                )
            generated_maps.append(json.loads(output.read_text(encoding="utf-8")))
        assert_case(
            root,
            "generated-comment-only-calls",
            generated_maps[0],
            generated_maps[1],
            "calls",
            0,
            repository=repository,
            node=node,
            addon=addon,
        )

        semantic_call = copy.deepcopy(before)
        semantic_call["symbol_calls"][0]["resolution"] = "unique"
        assert_case(
            root,
            "semantic-call",
            before,
            semantic_call,
            "calls",
            1,
            repository=repository,
            node=node,
            addon=addon,
        )

        export_origin = copy.deepcopy(before)
        origin = next(iter(export_origin["packages"][0]["export_origins"].values()))
        origin.append("new/origin.js")
        assert_case(
            root,
            "package-export-origin",
            before,
            export_origin,
            " public-api, public-api ",
            1,
            repository=repository,
            node=node,
            addon=addon,
        )

        file_only = copy.deepcopy(before)
        file_only["files"][0]["sha256"] = "f" * 64
        assert_case(
            root,
            "all-sections",
            before,
            file_only,
            "all",
            1,
            repository=repository,
            node=node,
            addon=addon,
        )
        assert_case(
            root,
            "default-excludes-file-content",
            before,
            file_only,
            None,
            0,
            repository=repository,
            node=node,
            addon=addon,
        )
        assert_case(
            root,
            "unknown-category",
            before,
            before,
            "unknown",
            2,
            repository=repository,
            node=node,
            addon=addon,
        )
    print("cross-host structural diff policy and position normalization passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
