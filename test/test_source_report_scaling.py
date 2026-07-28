#!/usr/bin/env python3
"""Exercise high-cardinality source selection validation and rendering."""

from __future__ import annotations

import hashlib
import json
from time import perf_counter

from archbird.native import Project, Source


def main() -> None:
    count = 10_000
    sources: list[Source] = []
    files: list[dict[str, object]] = []
    matched: list[dict[str, object]] = []
    for index in range(count):
        name = f"unit_{index:04d}"
        path = f"bulk/{name}.c"
        data = f"int {name}(void) {{ return {index}; }}\n".encode()
        symbol = {
            "extent": {"end": len(data), "start": 0},
            "kind": "function",
            "line": 1,
            "name": name,
            "scope": "local",
            "signature": f"int {name}(void)",
        }
        sources.append(Source(path, data, language="c", layer="c"))
        files.append(
            {
                "distance": 0,
                "language": "c",
                "path": path,
                "sha256": hashlib.sha256(data).hexdigest(),
                "symbols": [symbol],
            }
        )
        matched.append(
            {
                "kind": "function",
                "line": 1,
                "name": name,
                "path": path,
                "scope": "local",
            }
        )

    files.reverse()
    artifact = json.dumps(
        {
            "artifact": "query",
            "evidence": {
                "config_sha256": "0" * 64,
                "input_sha256": "1" * 64,
            },
            "files": files,
            "matched_symbols": matched,
            "project": "source-scaling",
            "query": {"plan": {"selection": {"paths": []}}},
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    project = Project("source-scaling", sources)

    started = perf_counter()
    first = project.source_markdown(artifact_json=artifact)
    elapsed = perf_counter() - started
    second = project.source_markdown(artifact_json=artifact)
    if first != second:
        raise AssertionError("high-cardinality source rendering is not repeatable")
    if first.count(b"\n### unit\\_") != count:
        raise AssertionError("high-cardinality source rendering lost declarations")
    if first.index(b"## bulk/unit\\_2499.c") > first.index(
        b"## bulk/unit\\_0000.c"
    ):
        raise AssertionError("source rendering discarded Query relevance order")
    if b"int unit_2499(void) { return 2499; }" not in first:
        raise AssertionError("source rendering omitted an exact declaration extent")
    if elapsed >= 2.0:
        raise AssertionError(
            f"high-cardinality source rendering exceeded 2s: {elapsed:.3f}s"
        )
    print(
        "source report scaling passed "
        f"({count} files/matches, {len(first)} bytes, {elapsed:.3f}s)"
    )


if __name__ == "__main__":
    main()
