#!/usr/bin/env python3
"""Exercise high-cardinality call resolution report aggregation."""

from __future__ import annotations

import json
from pathlib import Path
import statistics
import tempfile
from time import perf_counter

from archbird import _native
from archbird.native import (
    Project,
    evaluate_projection_json,
    render_map_markdown,
)


def _graph_map(document: dict[str, object], count: int) -> bytes:
    source_template = next(
        row for row in document["files"] if row["path"] == "src/core.c"
    )
    target_template = next(
        row for row in document["files"] if row["path"] == "src/core.h"
    )
    files = list(document["files"])
    edges = list(document["edges"])
    for index in range(count):
        source = dict(source_template)
        target = dict(target_template)
        source["path"] = f"bulk/consumer-{index:04d}.c"
        target["path"] = f"vendor/provider-{index:04d}.h"
        source["symbols"] = []
        target["symbols"] = []
        files.extend((source, target))
        edges.append(
            {
                "kind": "import",
                "names": [f"api_{index:04d}"],
                "source": source["path"],
                "target": target["path"],
            }
        )
    scaled = dict(document)
    scaled["files"] = files
    scaled["edges"] = edges
    return json.dumps(
        scaled, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode()


def _check_graph_aggregation(document: dict[str, object]) -> None:
    count = 2_500
    encoded = _graph_map(document, count)
    plan = {
        "id": "scaling-graph",
        "select": "graph",
        "group_by": "directory",
        "level": "file",
        "relations": ["imports"],
    }
    started = perf_counter()
    first = evaluate_projection_json(encoded, plan)
    elapsed = perf_counter() - started
    second = evaluate_projection_json(encoded, plan)
    if first != second:
        raise AssertionError("high-cardinality graph projection is not repeatable")
    result = json.loads(first)
    aggregate = next(
        row
        for row in result["fact"]["items"]
        if row["attributes"]["record_kind"] == "group_relation"
        and row["attributes"]["source"] == "directory:bulk"
        and row["attributes"]["target"] == "directory:vendor"
    )
    attributes = aggregate["attributes"]
    if (
        attributes["relation_count"] != count
        or attributes["witness_count"] != count
        or len(attributes["canonical_relation_keys"]) != count
        or len(attributes["names"]) != count
        or attributes["names"] != sorted(attributes["names"])
        or attributes["relation_kinds"] != ["import"]
    ):
        raise AssertionError(
            f"high-cardinality group relation is incomplete: {attributes!r}"
        )
    if elapsed >= 4.0:
        raise AssertionError(
            f"high-cardinality graph aggregation exceeded 4s: {elapsed:.3f}s"
        )
    report = render_map_markdown(encoded, detail="standard")
    if (
        len(report) >= 32_000
        or b"## Architecture groups\n" not in report
        or b"## Presentation accounting\n" not in report
        or b"presentation-omitted=" not in report
        or b"## Entities\n" in report
        or b"## Relations\n" in report
    ):
        raise AssertionError(
            "default high-cardinality Map report is not bounded and "
            "architecture-first"
        )
    print(
        f"high-cardinality graph projection passed in {elapsed:.3f}s "
        f"for {count} relations; standard report is {len(report)} bytes"
    )


def _edge_site_map(document: dict[str, object], count: int) -> bytes:
    source_template = next(
        row for row in document["files"] if row["path"] == "src/core.c"
    )
    target_template = next(
        row for row in document["files"] if row["path"] == "src/core.h"
    )
    sources: list[str] = []
    files = list(document["files"])
    edges = list(document["edges"])
    target = dict(target_template)
    target["path"] = "site-target/provider.h"
    target["symbols"] = []
    files.append(target)
    for index in range(count):
        path = f"site-source/consumer-{index:04d}.c"
        source = dict(source_template)
        source["path"] = path
        source["symbols"] = []
        files.append(source)
        sources.append(path)
        edges.append(
            {
                "kind": "import",
                "names": ["api"],
                "sites": [
                    {
                        "fact_id": f"f:edge-site-{index:04d}",
                        "line": 1,
                        "name": "api",
                        "path": path,
                        "span": {"end": 3, "start": 0},
                    }
                ],
                "source": path,
                "target": target["path"],
            }
        )
    scaled = dict(document)
    scaled["files"] = files
    scaled["edges"] = edges
    scaled["components"] = list(document["components"]) + [
        {
            "description": "",
            "files": sources,
            "name": "site-source",
            "outgoing": {},
            "symbol_count": 0,
        },
        {
            "description": "",
            "files": [target["path"]],
            "name": "site-target",
            "outgoing": {},
            "symbol_count": 0,
        },
    ]
    return json.dumps(
        scaled, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode()


def _check_edge_site_scaling(document: dict[str, object]) -> None:
    plan = {
        "id": "edge-site-scaling",
        "select": "component_edges",
        "kinds": ["import"],
    }
    elapsed: dict[int, float] = {}
    for count in (1_000, 2_000):
        encoded = _edge_site_map(document, count)
        outputs = []
        durations = []
        for _ in range(4):
            started = perf_counter()
            outputs.append(evaluate_projection_json(encoded, plan))
            durations.append(perf_counter() - started)
        if len(set(outputs)) != 1:
            raise AssertionError("edge-site projection is not repeatable")
        result = json.loads(outputs[-1])
        item = next(
            row
            for row in result["fact"]["items"]
            if row["label"] == "site-source -[import]-> site-target"
        )
        sites = item["attributes"]["sites"]
        if (
            result["completeness"]["classification"] != "complete"
            or len(sites) != count
            or len(item["evidence"]) != count
            or sites[0]["path"] != "site-source/consumer-0000.c"
            or sites[-1]["path"]
            != f"site-source/consumer-{count - 1:04d}.c"
        ):
            raise AssertionError("edge-site projection lost exact occurrences")
        elapsed[count] = statistics.median(durations[1:])
    if elapsed[2_000] > elapsed[1_000] * 2.8 + 0.02 or elapsed[2_000] >= 3.0:
        raise AssertionError(
            "edge-site projection no longer scales near-linearly: "
            f"1000={elapsed[1_000]:.3f}s 2000={elapsed[2_000]:.3f}s"
        )
    print(
        "edge-site projection scaling passed "
        f"(1000={elapsed[1_000]:.3f}s, 2000={elapsed[2_000]:.3f}s)"
    )


def _check_empty_summary() -> None:
    empty_root = Path(__file__).resolve().parents[1] / "build/test-empty-map-report"
    empty_root.mkdir(parents=True, exist_ok=True)
    project = Project.from_repository(
        empty_root,
        config=b"{}",
        cache_dir=empty_root / "cache",
    )
    report = render_map_markdown(project.map_json())
    tail = [line for line in report.splitlines() if line][-2:]
    expected = (
        b"Result: files=0; indexed-symbols=0; entities=0; relations=0; "
        b"diagnostics=0 (errors=0 warnings=0)."
    )
    if len(tail) != 2 or tail[0] != expected:
        raise AssertionError(f"empty Map report has an unstable tail: {tail!r}")


def _occurrence_map(count: int) -> bytes:
    config = json.dumps(
        {
            "project": f"occurrence-scaling-{count}",
            "layers": [
                {
                    "globs": ["*.py"],
                    "import_roots": ["."],
                    "language": "python",
                    "name": "python",
                }
            ],
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    build = Path(__file__).resolve().parents[1] / "build"
    with tempfile.TemporaryDirectory(
        prefix=f"occurrence-scaling-{count}-", dir=build
    ) as temporary:
        root = Path(temporary)
        (root / "api.py").write_text(
            "def old_api(value):\n    return value\n", encoding="utf-8"
        )
        for index in range(count):
            (root / f"caller_{index:04d}.py").write_text(
                "from api import old_api\n\n"
                f"def use_{index:04d}(value):\n"
                "    return old_api(value)\n",
                encoding="utf-8",
            )
        project = Project.from_repository(
            root,
            config=config,
            jobs=1,
            map_cache=False,
        )
        return project.map_json()


def _same_file_occurrence_map(count: int) -> tuple[bytes, float]:
    config = json.dumps(
        {
            "project": f"same-file-occurrence-scaling-{count}",
            "layers": [
                {
                    "globs": ["*.py"],
                    "import_roots": ["."],
                    "language": "python",
                    "name": "python",
                }
            ],
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    build = Path(__file__).resolve().parents[1] / "build"
    with tempfile.TemporaryDirectory(
        prefix=f"same-file-occurrence-scaling-{count}-", dir=build
    ) as temporary:
        root = Path(temporary)
        (root / "api.py").write_text(
            "def old_api(value):\n    return value\n", encoding="utf-8"
        )
        (root / "caller.py").write_text(
            "from api import old_api\n\n"
            "def use_all():\n"
            + "".join(f"    old_api({index})\n" for index in range(count)),
            encoding="utf-8",
        )
        started = perf_counter()
        project = Project.from_repository(
            root,
            config=config,
            jobs=1,
            map_cache=False,
        )
        mapped = project.map_json()
        return mapped, perf_counter() - started


def _check_symbol_occurrence_scaling() -> None:
    plan = {
        "id": "old-api-occurrences",
        "names": ["old_api"],
        "paths": ["api.py"],
        "select": "symbol_occurrences",
    }
    elapsed: dict[int, float] = {}
    for count in (400, 800):
        mapped = _occurrence_map(count)
        durations = []
        outputs = []
        for _ in range(6):
            started = perf_counter()
            outputs.append(evaluate_projection_json(mapped, plan))
            durations.append(perf_counter() - started)
        if len(set(outputs)) != 1:
            raise AssertionError("symbol occurrence projection is not repeatable")
        result = json.loads(outputs[-1])
        roles: dict[str, int] = {}
        for item in result["fact"]["items"]:
            role = item["attributes"]["role"]
            roles[role] = roles.get(role, 0) + 1
        if (
            result["completeness"]["classification"] != "complete"
            or roles
            != {
                "declaration": 1,
                "import": count,
                "reference": count,
            }
        ):
            raise AssertionError(
                f"symbol occurrence projection lost evidence: {roles!r}"
            )
        elapsed[count] = statistics.median(durations[1:])
    if elapsed[800] > elapsed[400] * 2.8 + 0.01 or elapsed[800] >= 2.0:
        raise AssertionError(
            "symbol occurrence projection no longer scales near-linearly: "
            f"400={elapsed[400]:.3f}s 800={elapsed[800]:.3f}s"
        )
    print(
        "symbol occurrence scaling passed "
        f"(400={elapsed[400]:.3f}s, 800={elapsed[800]:.3f}s)"
    )
    map_elapsed: dict[int, float] = {}
    for count in (2_000, 4_000):
        mapped, map_elapsed[count] = _same_file_occurrence_map(count)
        result = json.loads(evaluate_projection_json(mapped, plan))
        roles: dict[str, int] = {}
        for item in result["fact"]["items"]:
            role = item["attributes"]["role"]
            roles[role] = roles.get(role, 0) + 1
        if (
            result["completeness"]["classification"] != "complete"
            or roles
            != {
                "declaration": 1,
                "import": 1,
                "reference": count,
            }
        ):
            raise AssertionError(
                "same-file symbol occurrence projection lost evidence: "
                f"{roles!r}"
            )
    if (
        map_elapsed[4_000] > map_elapsed[2_000] * 2.7 + 0.02
        or map_elapsed[4_000] >= 2.0
    ):
        raise AssertionError(
            "same-file call correlation no longer scales near-linearly: "
            f"2000={map_elapsed[2_000]:.3f}s "
            f"4000={map_elapsed[4_000]:.3f}s"
        )
    print(
        "same-file call correlation scaling passed "
        f"(2000={map_elapsed[2_000]:.3f}s, "
        f"4000={map_elapsed[4_000]:.3f}s)"
    )


def main() -> int:
    fixture = Path(__file__).parent / "fixtures/report_map.json"
    document = json.loads(fixture.read_bytes())
    document["call_resolutions"] = [
        {
            "candidates": [],
            "count": count,
            "kind": "unresolved",
            "name": name,
            "source": source,
        }
        for name, count, source in (
            [("hot", 7, "js/index.js"), ("hot", 5, "js/main.js")]
            + [
                (f"unresolved-{index:04d}", 1, "js/index.js")
                for index in range(2000)
            ]
        )
    ]
    encoded = json.dumps(
        document, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode()
    first = _native.map_markdown(encoded)
    second = _native.map_markdown(encoded)
    if first != second:
        raise AssertionError("high-cardinality Map Markdown is not repeatable")
    if b"unresolved: hot(12)" not in first:
        raise AssertionError("Map Markdown did not aggregate resolution names")
    print("high-cardinality Map report aggregation passed")
    _check_graph_aggregation(document)
    _check_edge_site_scaling(document)
    _check_empty_summary()
    print("empty Map report summary passed")
    _check_symbol_occurrence_scaling()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
