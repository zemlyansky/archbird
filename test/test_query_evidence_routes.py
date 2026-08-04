#!/usr/bin/env python3
"""Keep Query routing evidence-dimensional and heuristic imports unproven."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import shutil
import sys


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


def endpoint(path: str) -> dict[str, object]:
    return {"kind": "file", "patterns": [path]}


def edge(document: dict, source: str, target: str) -> dict:
    matches = [
        row
        for row in document["edges"]
        if row["kind"] == "import"
        and row["source"] == source
        and row["target"] == target
    ]
    if len(matches) != 1:
        raise AssertionError(
            f"expected one {source} -> {target} import, got {matches!r}"
        )
    return matches[0]


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    root = repository / "build/query-evidence-routes"
    shutil.rmtree(root, ignore_errors=True)
    (root / "src").mkdir(parents=True)
    (root / "include/Acme").mkdir(parents=True)
    (root / "src/main.cpp").write_text(
        '#include "local.hpp"\n'
        "#include <Acme/detail.hpp>\n"
        "int main() { return detail(); }\n",
        encoding="utf-8",
    )
    (root / "src/local.hpp").write_text(
        '#include "../include/Acme/detail.hpp"\n', encoding="utf-8"
    )
    (root / "src/candidate.cpp").write_text(
        "#include <Acme/detail.hpp>\n"
        "int candidate() { return detail(); }\n",
        encoding="utf-8",
    )
    (root / "src/bridge.cpp").write_text(
        "int bridge() { return 0; }\n", encoding="utf-8"
    )
    (root / "src/heuristic.cpp").write_text(
        "#include <Acme/detail.hpp>\n"
        "int heuristic() { return 0; }\n",
        encoding="utf-8",
    )
    (root / "src/terminal.cpp").write_text(
        "int terminal() { return 0; }\n", encoding="utf-8"
    )
    (root / "include/Acme/detail.hpp").write_text(
        "inline int detail() { return 1; }\n", encoding="utf-8"
    )

    zero = Project.from_repository(
        root,
        project="query-evidence-routes",
        ignore=False,
        jobs=1,
        cache_dir=None,
        map_cache=False,
    )
    zero_map = zero.map()
    heuristic = edge(
        zero_map, "src/main.cpp", "include/Acme/detail.hpp"
    )
    expected_heuristic_evidence = [
        {
            "basis": "conventional-include-root",
            "provider": zero_map["discovery"]["profile"]["name"],
            "state": "unknown",
        }
    ]
    if heuristic.get("evidence") != expected_heuristic_evidence:
        raise AssertionError(
            "zero-config include/ match was not explicit candidate evidence"
        )

    query = zero.query(paths=["src/main.cpp"], depth=2, test_depth=0)
    routes = {row["path"]: row for row in query["files"]}
    if query["discovery"] != zero_map["discovery"]:
        raise AssertionError("Query collapsed Map discovery/completeness")
    ranked_paths = [row["path"] for row in query["files"]]
    if ranked_paths[:3] != [
        "src/main.cpp",
        "src/local.hpp",
        "include/Acme/detail.hpp",
    ] or (
        "src/candidate.cpp" in ranked_paths
        and ranked_paths.index("src/candidate.cpp")
        < ranked_paths.index("include/Acme/detail.hpp")
    ):
        raise AssertionError("stronger established route was not ranked first")
    detail_route = routes["include/Acme/detail.hpp"]["route"]
    if (
        routes["include/Acme/detail.hpp"]["distance"] != 2
        or detail_route["evidence_state"] != "current"
        or detail_route["resolution"]
        != {"ambiguous": 0, "candidate": 0, "unresolved": 0}
        or detail_route["provenance_count"] < 1
        or detail_route["via"]
        != {
            "kind": "import",
            "source": "src/local.hpp",
            "target": "include/Acme/detail.hpp",
            "traversal": "forward",
        }
    ):
        raise AssertionError("Query flattened or hid its strongest route")
    retained = edge(
        query, "src/main.cpp", "include/Acme/detail.hpp"
    )
    if retained.get("evidence") != expected_heuristic_evidence:
        raise AssertionError("Query deleted candidate evidence it traversed")

    stale_map = copy.deepcopy(zero_map)
    edge(stale_map, "src/main.cpp", "src/local.hpp")["evidence"] = [
        {
            "basis": "saved-source",
            "provider": "query-evidence-regression",
            "state": "stale",
        }
    ]
    stale_query = json.loads(
        query_map_json(
            json.dumps(
                stale_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            paths=["src/main.cpp"],
            depth=2,
            test_depth=0,
        )
    )
    stale_detail = next(
        row
        for row in stale_query["files"]
        if row["path"] == "include/Acme/detail.hpp"
    )
    if (
        stale_detail["distance"] != 1
        or stale_detail["route"]["evidence_state"] != "unknown"
        or stale_detail["route"]["via"]
        != {
            "kind": "import",
            "source": "src/main.cpp",
            "target": "include/Acme/detail.hpp",
            "traversal": "forward",
        }
    ):
        raise AssertionError(
            "Query preferred stale evidence over a current-source candidate: "
            f"{stale_detail!r}"
        )

    established_path = zero.path(
        endpoint("src/main.cpp"),
        endpoint("include/Acme/detail.hpp"),
        relations=["imports"],
    )
    if (
        established_path["outcome"] != "found"
        or established_path["search"]["shortest_length"] != 2
        or any(row["state"] != "current" for row in established_path["paths"])
    ):
        raise AssertionError("candidate shortcut hid the established Path")

    candidate_query = zero.query(
        paths=["src/heuristic.cpp"], depth=1, test_depth=0
    )
    candidate_detail = next(
        row
        for row in candidate_query["files"]
        if row["path"] == "include/Acme/detail.hpp"
    )
    if (
        candidate_detail["route"]["evidence_state"] != "unknown"
        or candidate_detail["route"]["resolution"]
        != {"ambiguous": 0, "candidate": 0, "unresolved": 0}
        or candidate_detail["route"]["provenance_count"] != 2
    ):
        raise AssertionError("Query promoted a heuristic-only route")

    semantic_query = zero.query(
        symbols=["src/candidate.cpp:candidate"], depth=1, test_depth=0
    )
    semantic_detail = next(
        row
        for row in semantic_query["files"]
        if row["path"] == "include/Acme/detail.hpp"
    )
    if (
        semantic_detail["route"]["evidence_state"] != "current"
        or semantic_detail["route"]["resolution"]
        != {"ambiguous": 0, "candidate": 1, "unresolved": 0}
        or semantic_detail["route"]["provenance_count"] < 1
        or semantic_detail["route"]["via"]
        != {
            "kind": "call",
            "source": "src/candidate.cpp",
            "target": "include/Acme/detail.hpp",
            "traversal": "forward",
        }
    ):
        raise AssertionError(
            "Query collapsed evidence state and semantic resolution"
        )

    template = next(
        row
        for row in zero_map["symbol_calls"]
        if row["source"]
        == {"path": "src/candidate.cpp", "symbol": "candidate"}
        and row["name"] == "detail"
    )

    def call(
        source_path: str,
        source_symbol: str,
        target_path: str,
        target_symbol: str,
        resolution: str,
    ) -> dict:
        row = copy.deepcopy(template)
        row["source"] = {"path": source_path, "symbol": source_symbol}
        row["name"] = target_symbol
        row["candidates"] = [
            {"line": 1, "path": target_path, "symbol": target_symbol}
        ]
        row["resolution"] = resolution
        return row

    unknown_symbol_map = copy.deepcopy(zero_map)
    unknown_symbol_call = call(
        "src/candidate.cpp",
        "candidate",
        "include/Acme/detail.hpp",
        "detail",
        "unique",
    )
    for evidence in unknown_symbol_call["evidence"]:
        evidence["state"] = "unknown"
    unknown_symbol_map["symbol_calls"] = [unknown_symbol_call]
    unknown_symbol_query = json.loads(
        query_map_json(
            json.dumps(
                unknown_symbol_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            symbols=["src/candidate.cpp:candidate"],
            direction="downstream",
            depth=1,
            test_depth=0,
        )
    )
    unknown_symbol_detail = next(
        row
        for row in unknown_symbol_query["files"]
        if row["path"] == "include/Acme/detail.hpp"
    )
    if (
        unknown_symbol_detail["route"]["evidence_state"] != "unknown"
        or unknown_symbol_detail["route"]["resolution"]
        != {"ambiguous": 0, "candidate": 0, "unresolved": 0}
        or unknown_symbol_detail["route"]["provenance_count"] < 1
    ):
        raise AssertionError("Query promoted unknown symbol-relation evidence")

    route_coupling_map = copy.deepcopy(zero_map)
    route_coupling_map["symbol_calls"] = [
        call(
            "src/candidate.cpp",
            "candidate",
            "include/Acme/detail.hpp",
            "detail",
            "candidate",
        ),
        call(
            "src/candidate.cpp",
            "candidate",
            "src/bridge.cpp",
            "bridge",
            "unique",
        ),
        call(
            "src/bridge.cpp",
            "bridge",
            "include/Acme/detail.hpp",
            "detail",
            "unique",
        ),
        call(
            "include/Acme/detail.hpp",
            "detail",
            "src/terminal.cpp",
            "terminal",
            "unique",
        ),
    ]
    encoded_coupling_map = json.dumps(
        route_coupling_map, sort_keys=True, separators=(",", ":")
    ).encode()
    bounded_coupling = json.loads(
        query_map_json(
            encoded_coupling_map,
            symbols=["src/candidate.cpp:candidate"],
            direction="downstream",
            depth=2,
            test_depth=0,
        )
    )
    if any(row["path"] == "src/terminal.cpp" for row in bounded_coupling["files"]):
        raise AssertionError(
            "Query combined a weak route distance with stronger route evidence"
        )
    complete_coupling = json.loads(
        query_map_json(
            encoded_coupling_map,
            symbols=["src/candidate.cpp:candidate"],
            direction="downstream",
            depth=3,
            test_depth=0,
        )
    )
    terminal_route = next(
        row["route"]
        for row in complete_coupling["files"]
        if row["path"] == "src/terminal.cpp"
    )
    if (
        terminal_route["evidence_state"] != "current"
        or terminal_route["resolution"]
        != {"ambiguous": 0, "candidate": 0, "unresolved": 0}
        or terminal_route["via"]
        != {
            "kind": "call",
            "source": "include/Acme/detail.hpp",
            "target": "src/terminal.cpp",
            "traversal": "forward",
        }
    ):
        raise AssertionError("Query route dimensions lost their selected path")

    candidate_path = zero.path(
        endpoint("src/heuristic.cpp"),
        endpoint("include/Acme/detail.hpp"),
        relations=["imports"],
    )
    if (
        candidate_path["outcome"] != "unknown"
        or candidate_path["reason"] != "candidate-witnesses"
        or candidate_path["paths"][0]["state"] != "unknown"
    ):
        raise AssertionError("Path promoted a heuristic-only route")

    explicit_config = {
        "project": "query-evidence-routes-explicit",
        "layers": [
            {
                "name": "cpp",
                "language": "cpp",
                "globs": ["include/**/*.hpp", "src/**/*.cpp", "src/**/*.hpp"],
                "import_roots": ["include"],
            }
        ],
    }
    explicit = Project.from_repository(
        root,
        config=json.dumps(
            explicit_config, sort_keys=True, separators=(",", ":")
        ).encode(),
        ignore=False,
        jobs=1,
        cache_dir=None,
        map_cache=False,
    )
    explicit_map = explicit.map()
    explicit_edge = edge(
        explicit_map, "src/heuristic.cpp", "include/Acme/detail.hpp"
    )
    if any(
        evidence.get("basis") == "conventional-include-root"
        for evidence in explicit_edge.get("evidence", [])
    ):
        raise AssertionError("authored import root retained heuristic evidence")
    explicit_query = explicit.query(
        paths=["src/heuristic.cpp"], depth=1, test_depth=0
    )
    explicit_detail = next(
        row
        for row in explicit_query["files"]
        if row["path"] == "include/Acme/detail.hpp"
    )
    explicit_path = explicit.path(
        endpoint("src/heuristic.cpp"),
        endpoint("include/Acme/detail.hpp"),
        relations=["imports"],
    )
    if (
        explicit_detail["distance"] != 1
        or explicit_detail["route"]["evidence_state"] != "current"
        or explicit_path["outcome"] != "found"
        or explicit_path["search"]["shortest_length"] != 1
    ):
        raise AssertionError("authored import root was not established evidence")

    shutil.rmtree(root)
    print(
        "Query evidence dimensions, candidate retention, and include-root "
        "provenance passed"
    )


if __name__ == "__main__":
    main()
