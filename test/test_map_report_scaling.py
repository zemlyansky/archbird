#!/usr/bin/env python3
"""Exercise high-cardinality call resolution report aggregation."""

from __future__ import annotations

import json
from pathlib import Path
from time import perf_counter

from archbird import _native
from archbird.native import evaluate_projection_json, render_map_markdown


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
        or b"Presentation omitted " not in report
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
