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
from archbird.native import (
    Project,
    diff_json,
    evaluate_projection_json,
)


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
        positional_before, positional_after = write_case(
            root, "positional", before, position_only
        )
        positional_results = {
            host: run_host(
                host,
                ["diff", str(positional_before), str(positional_after)],
                repository=repository,
                node=node,
                addon=addon,
            )
            for host in ("python", "node")
        }
        for host, result in positional_results.items():
            if result.returncode:
                raise AssertionError(
                    f"positional saved Diff failed for {host}: "
                    f"{result.stderr.decode(errors='replace')}"
                )
        if positional_results["python"].stdout != positional_results["node"].stdout:
            raise AssertionError("positional saved Diff diverged across hosts")

        config_path = root / "archbird.json"
        config_path.write_text('{"project":"not-a-map"}\n', encoding="utf-8")
        rejected = run_host(
            "python",
            ["diff", str(config_path), str(positional_after)],
            repository=repository,
            node=node,
            addon=addon,
        )
        if rejected.returncode != 2 or b"project configuration" not in rejected.stderr:
            raise AssertionError("Diff did not reject project configuration clearly")

        configured_roots = []
        for side in ("configured-before", "configured-after"):
            configured_root = root / side
            configured_root.mkdir()
            (configured_root / "rules.c").write_text(
                "typedef struct { int value; } Rule;\n"
                "static Rule rules[] = {{1}};\n",
                encoding="utf-8",
            )
            (configured_root / "archbird.json").write_text(
                json.dumps(
                    {
                        "layers": [
                            {
                                "globs": ["*.c"],
                                "language": "c",
                                "name": "c",
                            }
                        ],
                        "project": side,
                    }
                ),
                encoding="utf-8",
            )
            configured_roots.append(configured_root)
        structural_results = {
            host: run_host(
                host,
                [
                    "diff",
                    str(configured_roots[0]),
                    str(configured_roots[1]),
                    "--no-cache",
                    "--progress",
                    "never",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            for host in ("python", "node")
        }
        for host, result in structural_results.items():
            if result.returncode:
                raise AssertionError(
                    f"structural directory Diff failed for {host}: "
                    f"{result.stderr.decode(errors='replace')}"
                )
        if structural_results["python"].stdout != structural_results["node"].stdout:
            raise AssertionError("structural directory Diff diverged across hosts")
        configured_request = root / "configured-correspondence.json"
        configured_request.write_bytes(
            encode(
                {
                    "after": {
                        "container": "rules.c:rules",
                        "id": "after-rules",
                        "select": "ordered_rows",
                    },
                    "before": {
                        "container": "rules.c:rules",
                        "id": "before-rules",
                        "select": "ordered_rows",
                    },
                    "mapping": {},
                }
            )
        )
        configured_results = {
            host: run_host(
                host,
                [
                    "diff",
                    str(configured_roots[0]),
                    str(configured_roots[1]),
                    "--correspondence",
                    str(configured_request),
                    "--no-cache",
                    "--progress",
                    "never",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            for host in ("python", "node")
        }
        for host, result in configured_results.items():
            if result.returncode:
                raise AssertionError(
                    f"configured directory Diff failed for {host}: "
                    f"{result.stderr.decode(errors='replace')}"
                )
            document = json.loads(result.stdout)
            if document["before"]["map"]["project"] != "configured-before":
                raise AssertionError(f"{host} ignored the before project configuration")
            if document["after"]["map"]["project"] != "configured-after":
                raise AssertionError(f"{host} ignored the after project configuration")
            if document["comparison"]["state"] != "complete":
                raise AssertionError(f"{host} omitted configured ordered facts")
        if configured_results["python"].stdout != configured_results["node"].stdout:
            raise AssertionError("live project Diff diverged across hosts")

        for host in ("python", "node"):
            cache_dir = root / f"{host}-live-diff-cache"
            command = [
                "diff",
                str(configured_roots[0]),
                str(configured_roots[1]),
                "--correspondence",
                str(configured_request),
                "--cache-dir",
                str(cache_dir),
                "--progress",
                "never",
            ]
            cold = run_host(
                host,
                command,
                repository=repository,
                node=node,
                addon=addon,
            )
            warm = run_host(
                host,
                command,
                repository=repository,
                node=node,
                addon=addon,
            )
            if cold.returncode or warm.returncode:
                raise AssertionError(
                    f"cached directory Diff failed for {host}: "
                    f"{warm.stderr.decode(errors='replace')}"
                )
            if warm.stdout != cold.stdout:
                raise AssertionError(
                    f"Map-cache hit changed live directory Diff for {host}"
                )

        requirements = json.loads(
            _native.diff_requirements(configured_request.read_bytes())
        )
        if not all(requirements[side]["live_project"] for side in ("before", "after")):
            raise AssertionError("ordered correspondence did not select live projects")
        live_projects = []
        for index, side in enumerate(("before", "after")):
            configuration = json.loads(
                (configured_roots[index] / "archbird.json").read_text(
                    encoding="utf-8"
                )
            )
            configuration["ordered_containers"] = requirements[side][
                "ordered_containers"
            ]
            live_projects.append(
                Project.from_repository(
                    configured_roots[index],
                    config=encode(configuration),
                    jobs=1,
                    map_cache=False,
                )
            )
        direct = diff_json(
            live_projects[0],
            live_projects[1],
            correspondence=configured_request.read_bytes(),
            before_resolution=live_projects[0].resolution_json or b"",
            after_resolution=live_projects[1].resolution_json or b"",
        )
        materialized = diff_json(
            live_projects[0].map_json(),
            live_projects[1].map_json(),
            correspondence=configured_request.read_bytes(),
            before_resolution=live_projects[0].resolution_json or b"",
            after_resolution=live_projects[1].resolution_json or b"",
        )
        if direct != materialized:
            raise AssertionError("live and saved correspondence Diff diverged")
        direct_map_diff = diff_json(live_projects[0], live_projects[1])
        saved_map_diff = diff_json(
            live_projects[0].map_json(), live_projects[1].map_json()
        )
        if direct_map_diff != saved_map_diff:
            raise AssertionError("live and saved structural Diff diverged")

        cached_projects = []
        for index in range(2):
            configuration = json.loads(
                (configured_roots[index] / "archbird.json").read_text(
                    encoding="utf-8"
                )
            )
            configuration["ordered_containers"] = requirements[
                ("before", "after")[index]
            ]["ordered_containers"]
            cache_dir = root / f"direct-project-cache-{index}"
            warm_project = Project.from_repository(
                configured_roots[index],
                config=encode(configuration),
                jobs=1,
                cache_dir=cache_dir,
            )
            warm_project.map_json()
            cached_project = Project.from_repository(
                configured_roots[index],
                config=encode(configuration),
                jobs=1,
                cache_dir=cache_dir,
            )
            if cached_project.map_cache_stats["hits"] != 1:
                raise AssertionError("live Diff cache regression did not hit Map cache")
            cached_projects.append(cached_project)
        cached_direct = diff_json(
            cached_projects[0],
            cached_projects[1],
            correspondence=configured_request.read_bytes(),
            before_resolution=cached_projects[0].resolution_json or b"",
            after_resolution=cached_projects[1].resolution_json or b"",
        )
        if cached_direct != materialized:
            raise AssertionError("Map-cache hit changed live project Diff")

        fallback_request = root / "fallback-correspondence.json"
        fallback_request.write_bytes(
            encode(
                {
                    "after": {"id": "after-symbols", "select": "symbols"},
                    "before": {"id": "before-symbols", "select": "symbols"},
                    "mapping": {},
                }
            )
        )
        fallback_requirements = json.loads(
            _native.diff_requirements(fallback_request.read_bytes())
        )
        if any(
            fallback_requirements[side]["live_project"]
            for side in ("before", "after")
        ):
            raise AssertionError("symbol correspondence selected fact-only input")
        direct_fallback = diff_json(
            live_projects[0],
            live_projects[1],
            correspondence=fallback_request.read_bytes(),
            before_resolution=live_projects[0].resolution_json or b"",
            after_resolution=live_projects[1].resolution_json or b"",
        )
        saved_fallback = diff_json(
            live_projects[0].map_json(),
            live_projects[1].map_json(),
            correspondence=fallback_request.read_bytes(),
            before_resolution=live_projects[0].resolution_json or b"",
            after_resolution=live_projects[1].resolution_json or b"",
        )
        if direct_fallback != saved_fallback:
            raise AssertionError("project Diff fallback diverged from saved Maps")
        fallback_results = {
            host: run_host(
                host,
                [
                    "diff",
                    str(configured_roots[0]),
                    str(configured_roots[1]),
                    "--correspondence",
                    str(fallback_request),
                    "--no-cache",
                    "--progress",
                    "never",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            for host in ("python", "node")
        }
        for host, result in fallback_results.items():
            if result.returncode:
                raise AssertionError(
                    f"materialized correspondence fallback failed for {host}: "
                    f"{result.stderr.decode(errors='replace')}"
                )
        if fallback_results["python"].stdout != fallback_results["node"].stdout:
            raise AssertionError("materialized correspondence fallback diverged")

        unsupported_root = root / "unsupported-only"
        unsupported_root.mkdir()
        (unsupported_root / "lib.rs").write_text(
            "pub fn open() {}\n", encoding="utf-8"
        )
        for host in ("python", "node"):
            result = run_host(
                host,
                [
                    "map",
                    str(unsupported_root),
                    "--no-config",
                    "--no-cache",
                    "--progress",
                    "never",
                    "--check",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            if (
                result.returncode != 1
                or b"discovery-no-supported-source" not in result.stdout
            ):
                raise AssertionError(
                    f"{host} checked Map accepted an unsupported-only repository"
                )

        ordered_root = root / "ordered"
        (ordered_root / "py").mkdir(parents=True)
        (ordered_root / "c").mkdir()
        (ordered_root / "py/rules.py").write_text(
            "rules = [('first', 1), ('second', 2)]\n",
            encoding="utf-8",
        )
        (ordered_root / "c/rules.c").write_text(
            "typedef struct { int value; } Rule;\n"
            "static Rule rules[] = {{1}, {2}};\n",
            encoding="utf-8",
        )
        ordered_config = {
            "layers": [
                {"globs": ["py/**/*.py"], "language": "python", "name": "py"},
                {"globs": ["c/**/*.c"], "language": "c", "name": "c"},
            ],
            "ordered_containers": [
                {"language": "python", "names": ["rules"], "paths": ["py/**"]},
                {"language": "c", "names": ["rules"], "paths": ["c/**"]},
            ],
            "project": "ordered-diff",
        }
        ordered_project = Project.from_repository(
            ordered_root,
            config=encode(ordered_config),
            jobs=1,
            map_cache=False,
        )
        ordered_map = ordered_project.map_json()
        before_definition = {
            "container": "py/rules.py:rules",
            "id": "before-rules",
            "select": "ordered_rows",
        }
        after_definition = {
            "container": "c/rules.c:rules",
            "id": "after-rules",
            "select": "ordered_rows",
        }
        before_projection = json.loads(
            evaluate_projection_json(ordered_map, before_definition)
        )
        after_projection = json.loads(
            evaluate_projection_json(ordered_map, after_definition)
        )
        before_keys = sorted(
            (row["key"] for row in before_projection["fact"]["items"]),
            key=lambda key: key,
        )
        after_keys = sorted(
            (row["key"] for row in after_projection["fact"]["items"]),
            key=lambda key: key,
        )
        if len(before_keys) != 2 or len(after_keys) != 2:
            raise AssertionError("ordered Diff fixture did not expose two row groups")
        correspondence = {
            "after": after_definition,
            "before": before_definition,
            "mapping": {
                before_keys[0]: [after_keys[0], after_keys[1]],
                before_keys[1]: [after_keys[1]],
            },
        }
        ordered_map_path = root / "ordered-map.json"
        correspondence_path = root / "correspondence.json"
        ordered_map_path.write_bytes(ordered_map)
        correspondence_path.write_bytes(encode(correspondence))
        mapped_results = {
            host: run_host(
                host,
                [
                    "diff",
                    str(ordered_map_path),
                    str(ordered_map_path),
                    "--correspondence",
                    str(correspondence_path),
                    "--check",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            for host in ("python", "node")
        }
        for host, result in mapped_results.items():
            if result.returncode:
                raise AssertionError(
                    f"mapped Diff failed for {host}: "
                    f"{result.stderr.decode(errors='replace')}"
                )
        if mapped_results["python"].stdout != mapped_results["node"].stdout:
            raise AssertionError("mapped Diff diverged across hosts")
        mapped = json.loads(mapped_results["python"].stdout)
        if mapped["comparison"]["counts"]["matched"] != 3:
            raise AssertionError("mapped Diff lost one-to-many correspondence")
        for host in ("python", "node"):
            categorized = run_host(
                host,
                [
                    "diff",
                    str(ordered_map_path),
                    str(ordered_map_path),
                    "--correspondence",
                    str(correspondence_path),
                    "--check=public-api",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            if categorized.returncode != 2 or b"risk categories" not in categorized.stderr:
                raise AssertionError(
                    f"mapped Diff accepted risk categories for {host}: "
                    f"{categorized.stderr.decode(errors='replace')}"
                )

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
        live_results = {
            host: run_host(
                host,
                [
                    "diff",
                    str(comment_root),
                    str(comment_root),
                    "--no-cache",
                    "--progress",
                    "never",
                ],
                repository=repository,
                node=node,
                addon=addon,
            )
            for host in ("python", "node")
        }
        for host, result in live_results.items():
            if result.returncode:
                raise AssertionError(
                    f"live root Diff failed for {host}: "
                    f"{result.stderr.decode(errors='replace')}"
                )
            if any(
                any(values for values in section.values())
                for section in json.loads(result.stdout)["sections"].values()
            ):
                raise AssertionError(f"live root self-Diff changed for {host}")
        saved_comment_map = root / "comment-saved.json"
        saved_comment_map.write_bytes(encode(generated_maps[1]))
        mixed = run_host(
            "python",
            [
                "diff",
                str(comment_root),
                str(saved_comment_map),
                "--no-cache",
                "--progress",
                "never",
            ],
            repository=repository,
            node=node,
            addon=addon,
        )
        if mixed.returncode:
            raise AssertionError(
                f"mixed live/saved Diff failed: {mixed.stderr.decode(errors='replace')}"
            )
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
