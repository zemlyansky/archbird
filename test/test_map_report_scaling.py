#!/usr/bin/env python3
"""Exercise high-cardinality call resolution report aggregation."""

from __future__ import annotations

import json
from pathlib import Path

from archbird import _native


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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
