#!/usr/bin/env python3
"""Keep bounded Query traversal proportional to reachable adjacency."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import statistics
import sys
from time import perf_counter


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


if len(sys.argv) > 1:
    load_extension(Path(sys.argv[1]).resolve())

from archbird.native import Project, query_map_json  # noqa: E402


def measured_query(
    map_json: bytes, depth: int, test_depth: int
) -> tuple[bytes, float]:
    started = perf_counter()
    output = query_map_json(
        map_json,
        paths=["seed.js"],
        depth=depth,
        test_depth=test_depth,
    )
    return output, perf_counter() - started


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    root = repository / "build/query-adjacency-scaling"
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    (root / "seed.js").write_text('import "./reachable.js";\n', encoding="utf-8")
    (root / "reachable.js").write_text("export const value = 1;\n", encoding="utf-8")
    try:
        project = Project.from_repository(
            root,
            project="query-adjacency-scaling",
            ignore=False,
            jobs=1,
            cache_dir=None,
            map_cache=False,
        )
        document = project.map()
        template_file = copy.deepcopy(document["files"][0])
        if not any(
            edge["source"] == "seed.js" and edge["target"] == "reachable.js"
            for edge in document["edges"]
        ):
            raise AssertionError("adjacency scaling seed edge is missing")
        disconnected_files = int(
            os.environ.get("ARCHBIRD_QUERY_ADJACENCY_FILES", "4096")
        )
        if disconnected_files < 32:
            raise AssertionError("query adjacency file fixture is too small")
        for index in range(disconnected_files):
            row = copy.deepcopy(template_file)
            row["path"] = f"disconnected-{index:02d}.js"
            row["symbols"] = []
            document["files"].append(row)
        document["files"].sort(key=lambda row: row["path"].encode())

        unrelated_edges = int(
            os.environ.get("ARCHBIRD_QUERY_ADJACENCY_EDGES", "100000")
        )
        if unrelated_edges < 1000:
            raise AssertionError("query adjacency fixture is too small")
        for index in range(unrelated_edges):
            document["edges"].append(
                {
                    "kind": "import",
                    "names": [f"unrelated-{index:05d}"],
                    "source": "disconnected-00.js",
                    "target": "disconnected-01.js",
                }
            )
        encoded = json.dumps(
            document, sort_keys=True, separators=(",", ":")
        ).encode()

        shallow_output, _ = measured_query(encoded, 1, 0)
        deep_output, _ = measured_query(encoded, 16, 0)
        shallow_document = json.loads(shallow_output)
        deep_document = json.loads(deep_output)
        if [row["path"] for row in shallow_document["files"]] != [
            "seed.js",
            "reachable.js",
        ]:
            raise AssertionError("adjacency scaling seed component changed")
        if shallow_document["files"] != deep_document["files"]:
            raise AssertionError(
                "disconnected edges changed the bounded Query result"
            )

        samples: dict[str, list[float]] = {
            "shallow": [],
            "deep": [],
            "reverse": [],
        }
        outputs: dict[str, list[bytes]] = {
            "shallow": [],
            "deep": [],
            "reverse": [],
        }
        for _ in range(3):
            for label, depth, test_depth in (
                ("shallow", 1, 0),
                ("deep", 16, 0),
                ("reverse", 1, 1),
            ):
                output, elapsed = measured_query(encoded, depth, test_depth)
                outputs[label].append(output)
                samples[label].append(elapsed)
        for label in samples:
            if any(
                output != outputs[label][0] for output in outputs[label][1:]
            ):
                raise AssertionError(
                    f"{label} adjacency Query is not deterministic"
                )
        shallow = statistics.median(samples["shallow"])
        deep = statistics.median(samples["deep"])
        reverse = statistics.median(samples["reverse"])
        if deep > shallow * 2.5 + 0.05:
            raise AssertionError(
                "Query traversal scales with unrelated edges at every depth: "
                f"depth-1={shallow:.3f}s depth-16={deep:.3f}s "
                f"unrelated-edges={unrelated_edges}"
            )
        if reverse > shallow * 2.5 + 0.05:
            raise AssertionError(
                "reverse test routing scales with every file-edge pair: "
                f"test-depth-0={shallow:.3f}s "
                f"test-depth-1={reverse:.3f}s "
                f"files={disconnected_files + 2} "
                f"unrelated-edges={unrelated_edges}"
            )
        print(
            "query adjacency scaling passed "
            f"(depth-1={shallow:.3f}s; depth-16={deep:.3f}s; "
            f"test-depth-1={reverse:.3f}s; "
            f"files={disconnected_files + 2}; "
            f"unrelated-edges={unrelated_edges})"
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
