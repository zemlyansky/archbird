#!/usr/bin/env python3
"""Adversarial connection-path tests for every Python native host."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


if len(sys.argv) > 1:
    load_extension(Path(sys.argv[1]))

from archbird.native import Project, render_path_markdown  # noqa: E402
from archbird.cli import main as cli_main  # noqa: E402
from archbird.schema import read_schema, schema_names  # noqa: E402


def endpoint(patterns: list[str]) -> dict[str, object]:
    return {"kind": "file", "patterns": patterns}


def assert_path_shape(document: dict[str, object]) -> None:
    paths = document["paths"]
    search = document["search"]
    if not isinstance(paths, list) or not isinstance(search, dict):
        raise AssertionError("Path artifact arrays are malformed")
    if search["path_count"] != len(paths):
        raise AssertionError("Path search count diverges from canonical paths")
    if paths and any(
        path["length"] != search["shortest_length"] for path in paths
    ):
        raise AssertionError("Path artifact contains a non-shortest witness")
    if not paths and search["shortest_length"] is not None:
        raise AssertionError("empty Path artifact claims a shortest length")
    for path in paths:
        if path["state"] not in {"current", "unknown"}:
            raise AssertionError("Path witness has no evidence state")
        if path["length"] != len(path["steps"]):
            raise AssertionError("Path length diverges from typed steps")
        if len(path["nodes"]) != path["length"] + 1:
            raise AssertionError("Path nodes do not bound every typed step")
        for step in path["steps"]:
            relation = step["relation"]
            if (
                step["traversal"] not in {"forward", "reverse"}
                or relation["attributes"]["record_kind"] != "relation"
                or not relation["evidence"]
            ):
                raise AssertionError("Path step lost relation direction or evidence")


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    build = repository / "build"
    build.mkdir(exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="path-test-", dir=build))
    artifacts = root.with_name(f"{root.name}-artifacts")
    artifacts.mkdir()
    try:
        sources = {
            "a.js": (
                'import { b } from "./b.js";\n'
                'import "./c.js";\n'
                "export function a() { return b() + d(); }\n"
            ),
            "b.js": 'import "./d.js";\nexport function b() { return 1; }\n',
            "c.js": 'import "./d.js";\n',
            "d.js": "export function d() { return 1; }\n",
        }
        for name, source in sources.items():
            (root / name).write_text(source, encoding="utf-8")
        project = Project.from_repository(
            root, cache_dir=None, map_cache=False, project="path-test"
        )

        equal = project.path(
            endpoint(["a.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
        )
        assert_path_shape(equal)
        if (
            equal["artifact"] != "path"
            or equal["schema_version"] != 1
            or equal["outcome"] != "found"
            or equal["reason"] != "witnesses"
            or [path["nodes"] for path in equal["paths"]]
            != [
                ["file:a.js", "file:b.js", "file:d.js"],
                ["file:a.js", "file:c.js", "file:d.js"],
            ]
        ):
            raise AssertionError("equal shortest witnesses are not deterministic")

        parallel = project.path(
            endpoint(["a.js"]),
            endpoint(["b.js"]),
            relations=["calls", "imports", "references"],
        )
        assert_path_shape(parallel)
        parallel_families = [
            path["steps"][0]["relation"]["attributes"]["family"]
            for path in parallel["paths"]
        ]
        if (
            parallel["search"]["shortest_length"] != 1
            or "imports" not in parallel_families
            or any(path["state"] != "current" for path in parallel["paths"])
        ):
            raise AssertionError("proven typed relation was not retained")

        proof_over_shortcut = project.path(
            endpoint(["a.js"]),
            endpoint(["d.js"]),
            relations=["calls", "imports"],
        )
        assert_path_shape(proof_over_shortcut)
        if (
            proof_over_shortcut["outcome"] != "found"
            or proof_over_shortcut["search"]["shortest_length"] != 2
            or any(
                path["state"] != "current"
                for path in proof_over_shortcut["paths"]
            )
            or any(
                step["relation"]["attributes"]["family"] != "imports"
                for path in proof_over_shortcut["paths"]
                for step in path["steps"]
            )
        ):
            raise AssertionError("short candidate shortcut hid a proven path")

        symbol_labels = project.path(
            {"kind": "symbol", "patterns": ["a"]},
            {"kind": "symbol", "patterns": ["b"]},
            level="symbol",
            relations=["calls"],
        )
        assert_path_shape(symbol_labels)
        if (
            symbol_labels["outcome"] != "unknown"
            or symbol_labels["reason"] != "candidate-witnesses"
            or symbol_labels["search"]["shortest_length"] != 1
            or symbol_labels["source"]["candidates"][0]["label"] != "a"
            or symbol_labels["target"]["candidates"][0]["label"] != "b"
            or symbol_labels["paths"][0]["state"] != "unknown"
            or symbol_labels["paths"][0]["steps"][0]["relation"]["state"]
            != "unknown"
        ):
            raise AssertionError(
                "candidate-only symbol route was promoted to proven"
            )

        reverse_only = project.path(
            endpoint(["d.js"]),
            endpoint(["a.js"]),
            relations=["imports"],
        )
        if (
            reverse_only["outcome"] != "absent"
            or reverse_only["reason"] != "exhaustive"
            or reverse_only["paths"]
        ):
            raise AssertionError("directed reverse-only path was invented")
        upstream = project.path(
            endpoint(["d.js"]),
            endpoint(["a.js"]),
            relations=["imports"],
            direction="upstream",
        )
        assert_path_shape(upstream)
        if (
            len(upstream["paths"]) != 2
            or any(
                step["traversal"] != "reverse"
                for path in upstream["paths"]
                for step in path["steps"]
            )
        ):
            raise AssertionError("upstream traversal lost canonical edge direction")

        ambiguous = project.path(
            endpoint(["b.js", "c.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
        )
        assert_path_shape(ambiguous)
        if ambiguous["source"]["state"] != "ambiguous" or len(
            ambiguous["source"]["candidates"]
        ) != 2:
            raise AssertionError("endpoint ambiguity was collapsed")

        globally_shortest = project.path(
            endpoint(["a.js", "b.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
        )
        assert_path_shape(globally_shortest)
        if (
            globally_shortest["search"]["shortest_length"] != 1
            or [path["nodes"] for path in globally_shortest["paths"]]
            != [["file:b.js", "file:d.js"]]
        ):
            raise AssertionError(
                "ambiguous endpoint sets retained non-global shortest paths"
            )

        bounded = project.path(
            endpoint(["a.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
            max_depth=1,
        )
        if (
            bounded["outcome"] != "unknown"
            or bounded["reason"] != "depth-frontier"
            or not bounded["search"]["frontier"]
        ):
            raise AssertionError("bounded frontier was reported as absence")

        limited = project.path(
            endpoint(["a.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
            max_paths=1,
        )
        assert_path_shape(limited)
        if (
            limited["reason"] != "path-limit"
            or not limited["search"]["truncated"]
            or len(limited["paths"]) != 1
        ):
            raise AssertionError("path limit did not retain explicit truncation")

        unresolved = project.path(
            endpoint(["missing.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
        )
        if (
            unresolved["outcome"] != "unknown"
            or unresolved["reason"] != "endpoint-unresolved"
            or unresolved["source"]["state"] != "unresolved"
        ):
            raise AssertionError("unresolved endpoint was reported as absence")

        normalized_left = project.path(
            endpoint(["missing.js", "a.js"]),
            endpoint(["d.js"]),
            relations=["references", "imports"],
        )
        normalized_right = project.path(
            endpoint(["a.js", "missing.js"]),
            endpoint(["d.js"]),
            relations=["imports", "references"],
        )
        if (
            normalized_left["path_sha256"] != normalized_right["path_sha256"]
            or normalized_left["request"] != normalized_right["request"]
        ):
            raise AssertionError("set-like Path request fields are not normalized")

        (root / "unsupported.go").write_text(
            "package incomplete\n", encoding="utf-8"
        )
        incomplete_project = Project.from_repository(
            root, cache_dir=None, map_cache=False, project="path-test"
        )
        incomplete = incomplete_project.path(
            endpoint(["d.js"]),
            endpoint(["a.js"]),
            relations=["imports"],
        )
        if (
            incomplete["outcome"] != "unknown"
            or incomplete["reason"] != "graph-incomplete"
            or incomplete["graph"]["classification"] != "incomplete"
        ):
            raise AssertionError("incomplete repository coverage proved absence")

        canonical_markdown_path = project.path_json(
            endpoint(["a.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
        )
        markdown_bytes = project.path_markdown(
            endpoint(["a.js"]),
            endpoint(["d.js"]),
            relations=["imports"],
        )
        if markdown_bytes != render_path_markdown(canonical_markdown_path):
            raise AssertionError("Path artifact renderer changed the witness")
        markdown = markdown_bytes.decode("utf-8")
        if (
            "# Connection paths: path-test" not in markdown
            or markdown.count("## Witness") != 2
            or "`imports:import` traversed forward" not in markdown
            or "state `current`" not in markdown
            or "resolution `not-applicable`" not in markdown
            or "evidence=" not in markdown
            or "provenance=derived" not in markdown
        ):
            raise AssertionError("Path Markdown hides canonical evidence")

        map_path = artifacts / "map.json"
        live_path = artifacts / "live-path.json"
        saved_path = artifacts / "saved-path.json"
        map_path.write_bytes(project.map_json())
        live_status = cli_main(
            [
                "path",
                "a.js",
                "d.js",
                "--root",
                str(root),
                "--no-config",
                "--no-cache",
                "--relation",
                "imports",
                "--format",
                "json",
                "--progress",
                "never",
                "--check",
                "--output",
                str(live_path),
            ]
        )
        saved_status = cli_main(
            [
                "path",
                "a.js",
                "d.js",
                "--map",
                str(map_path),
                "--relation",
                "imports",
                "--format",
                "json",
                "--progress",
                "never",
                "--check",
                "--output",
                str(saved_path),
            ]
        )
        if (
            live_status != 0
            or saved_status != 0
            or json.loads(live_path.read_bytes())["outcome"] != "found"
            or json.loads(saved_path.read_bytes())["outcome"] != "found"
        ):
            raise AssertionError("live or saved-Map Path CLI failed")
        candidate_check_status = cli_main(
            [
                "path",
                "a",
                "b",
                "--map",
                str(map_path),
                "--level",
                "symbol",
                "--relation",
                "calls",
                "--format",
                "markdown",
                "--progress",
                "never",
                "--check",
            ]
        )
        if candidate_check_status != 1:
            raise AssertionError("candidate-only checked Markdown succeeded")
        invalid_stderr = io.StringIO()
        with contextlib.redirect_stderr(invalid_stderr):
            invalid_status = cli_main(
                [
                    "path",
                    "a.js",
                    "d.js",
                    "--map",
                    str(map_path),
                    "--config",
                    "ignored.json",
                    "--format",
                    "json",
                ]
            )
        if invalid_status != 2 or "--map cannot be combined" not in (
            invalid_stderr.getvalue()
        ):
            raise AssertionError("saved-Map Path silently ignored --config")

        names = schema_names()
        if "path-request.schema.json" not in names or "path.schema.json" not in names:
            raise AssertionError("Path schemas are absent from package inventory")
        for name in ("path-request.schema.json", "path.schema.json"):
            schema = json.loads(read_schema(name))
            if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
                raise AssertionError(f"{name} is not a public draft-2020-12 schema")
    finally:
        shutil.rmtree(root)
        shutil.rmtree(artifacts)
    print("evidence-preserving connection paths passed")


if __name__ == "__main__":
    main()
