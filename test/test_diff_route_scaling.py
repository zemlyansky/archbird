#!/usr/bin/env python3
"""Bound structural Diff aggregation for repeated test-route evidence."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
from time import perf_counter

from archbird import _native


def encode(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def route_map(repository: Path, count: int, *, unique: bool) -> bytes:
    document = json.loads(
        (repository / "test/fixtures/report_map.json").read_text(encoding="utf-8")
    )
    test = document["tests"][0]
    cases = []
    for index in range(count):
        cases.append(
            {
                "configured_routes": [],
                "line": index + 1,
                "route_evidence": [
                    {
                        "claim": "syntax-reference",
                        "enclosing": "",
                        "fact_id": f"route-{index:06d}",
                        "line": index + 1,
                        "name": "target",
                        "provenance": "derived",
                        "provider": "fixture",
                        "relation": "call",
                        "scope": "case",
                        "span": {"end": index * 2 + 1, "start": index * 2},
                        "target": "src/target.c",
                        "target_symbol": "target",
                    }
                ],
                "routes": {"src/target.c": 1},
                "selector": f"case:{index:06d}" if unique else "case:shared",
            }
        )
    test["cases"] = cases
    test["count"] = count
    return encode(document)


def maximum_rss_kib() -> int:
    rss = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    return rss // 1024 if sys.platform == "darwin" else rss


def worker(repository: Path, count: int, unique: bool) -> None:
    mapped = route_map(repository, count, unique=unique)
    started = perf_counter()
    result = _native.map_diff(mapped, mapped)
    elapsed = perf_counter() - started
    section = json.loads(result)["sections"]["test_route_evidence"]
    if any(section.values()):
        raise AssertionError("self-Diff reported repeated route changes")
    print(
        json.dumps(
            {
                "elapsed": elapsed,
                "rss_kib": maximum_rss_kib(),
                "sha256": hashlib.sha256(result).hexdigest(),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
    )


def run_worker(repository: Path, count: int, unique: bool) -> dict[str, object]:
    completed = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "--worker",
            str(repository),
            str(count),
            "unique" if unique else "repeated",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    result = json.loads(completed.stdout)
    if (
        not isinstance(result.get("elapsed"), (int, float))
        or not isinstance(result.get("rss_kib"), int)
        or not isinstance(result.get("sha256"), str)
    ):
        raise AssertionError("Diff scaling worker returned invalid measurements")
    return result


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        if len(sys.argv) != 5 or sys.argv[4] not in {"repeated", "unique"}:
            raise SystemExit(
                "usage: test_diff_route_scaling.py "
                "--worker REPOSITORY COUNT repeated|unique"
            )
        worker(Path(sys.argv[2]), int(sys.argv[3]), sys.argv[4] == "unique")
        return
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_diff_route_scaling.py REPOSITORY")
    repository = Path(sys.argv[1]).resolve()
    large = int(os.environ.get("ARCHBIRD_DIFF_ROUTE_ROWS", "100000"))
    if large < 2:
        raise AssertionError("Diff route scaling row count must be at least two")
    small = max(1, large // 2)

    small_result = run_worker(repository, small, False)
    large_result = run_worker(repository, large, False)
    repeated_result = run_worker(repository, large, False)
    unique_result = run_worker(repository, large, True)
    if large_result["sha256"] != repeated_result["sha256"]:
        raise AssertionError("repeated route Diff is not deterministic")
    if large_result["elapsed"] > small_result["elapsed"] * 3.0 + 0.5:
        raise AssertionError(
            "repeated route Diff no longer scales near O(W log W): "
            f"{small}={small_result['elapsed']:.3f}s "
            f"{large}={large_result['elapsed']:.3f}s"
        )
    rss_allowance = int(unique_result["rss_kib"] * 1.15) + 8192
    if repeated_result["rss_kib"] > rss_allowance:
        raise AssertionError(
            "repeated route grouping exceeds unique-route memory: "
            f"repeated={repeated_result['rss_kib']} KiB "
            f"unique={unique_result['rss_kib']} KiB "
            f"allowance={rss_allowance} KiB"
        )
    print(
        "repeated route Diff scaling passed "
        f"({small}={small_result['elapsed']:.3f}s; "
        f"{large}={large_result['elapsed']:.3f}s; "
        f"repeated-rss={repeated_result['rss_kib']} KiB; "
        f"unique-rss={unique_result['rss_kib']} KiB)"
    )


if __name__ == "__main__":
    main()
