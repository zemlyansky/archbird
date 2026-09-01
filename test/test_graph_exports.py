from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

from archbird.interchange.graph.graphml import render_graphml
from archbird.interchange.graph.mermaid import render_mermaid
from archbird.interchange.graph.model import project_graph
from archbird.map.analyze import analyze
from archbird.map.config import load_config
from archbird.map.render import render_json


def run(binary: Path, map_json: str, format_name: str, view: str, direction="LR"):
    process = subprocess.run(
        [str(binary), format_name, view, direction],
        input=map_json.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode:
        raise AssertionError(process.stderr.decode(errors="replace"))
    return process.stdout.decode()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_graph_exports.py NATIVE_GRAPH CONFIG")
    binary = Path(sys.argv[1]).resolve()
    data = analyze(load_config(Path(sys.argv[2]).resolve()))
    map_json = render_json(data)
    for view in ("components", "files"):
        graph = project_graph(data, view=view)
        expected_graphml = render_graphml(graph)
        first = run(binary, map_json, "graphml", view)
        second = run(binary, map_json, "graphml", view)
        assert first == second == expected_graphml
        for direction in ("BT", "LR", "RL", "TB"):
            expected_mermaid = render_mermaid(graph, direction=direction)
            first = run(binary, map_json, "mermaid", view, direction)
            second = run(binary, map_json, "mermaid", view, direction)
            assert first == second == expected_mermaid
        first_json = run(binary, map_json, "json", view)
        second_json = run(binary, map_json, "json", view)
        assert first_json == second_json
        document = json.loads(first_json)
        assert document["artifact"] == "archbird-graph-view"
        assert document["request"]["view"] == view
        assert document["source"]["artifact"] == "map"
        assert document["summary"] == {
            "edges": len(document["edges"]),
            "nodes": len(document["nodes"]),
        }
        ids = {node["id"] for node in document["nodes"]}
        assert len(ids) == len(document["nodes"])
        assert all(edge["source"] in ids and edge["target"] in ids for edge in document["edges"])

    documented_map = json.loads(map_json)
    documented_map["files"][0]["documentation"] = {
        "kind": "doxygen",
        "span": {"end": 24, "start": 0},
        "text": "Document the mapped file.",
    }
    documented_file_graph = json.loads(
        run(binary, json.dumps(documented_map), "json", "files")
    )
    documented_file_nodes = [
        node
        for node in documented_file_graph["nodes"]
        if node["kind"] == "file"
        and node["label"] == documented_map["files"][0]["path"]
    ]
    assert len(documented_file_nodes) == 1
    assert documented_file_nodes[0]["attributes"]["documentation"] == (
        documented_map["files"][0]["documentation"]
    )

    query_json = (Path(__file__).parent / "fixtures/report_query.json").read_text()
    first = run(binary, query_json, "json", "symbols")
    second = run(binary, query_json, "json", "symbols")
    assert first == second
    document = json.loads(first)
    assert document["request"]["view"] == "symbols"
    assert document["source"]["artifact"] == "query"
    assert {node["kind"] for node in document["nodes"]} == {
        "builtin",
        "file",
        "symbol",
        "unresolved",
    }
    assert {edge["classification"] for edge in document["edges"]} == {
        "builtin",
        "unique",
        "unresolved",
    }
    assert document["omissions"] == [
        {"count": 1, "kind": "test-matches", "reason": "not-symbol-edges"}
    ]
    files = [node for node in document["nodes"] if node["kind"] == "file"]
    assert all("symbols" not in node["attributes"] for node in files)
    assert all(
        node["parent"] is None or node["parent"] in {row["id"] for row in files}
        for node in document["nodes"]
    )

    # Path queries carry declarations in files[].symbols without exact matches.
    path_query = json.loads(query_json)
    path_query["matched_symbols"] = []
    path_query["symbol_calls"] = []
    path_query["symbol_references"] = []
    documented = path_query["files"][0]["symbols"][0]
    path_query["files"][0]["documentation"] = {
        "kind": "doxygen",
        "span": {"end": 24, "start": 0},
        "text": "Core arithmetic module.",
    }
    documented["documentation"] = {
        "kind": "doxygen",
        "span": {"end": 25, "start": 0},
        "text": "Multiply two core values.",
    }
    path_document = json.loads(
        run(binary, json.dumps(path_query), "json", "symbols")
    )
    documented_nodes = [
        node
        for node in path_document["nodes"]
        if node["kind"] == "symbol" and node["label"] == "core_mul"
    ]
    assert len(documented_nodes) == 1
    assert documented_nodes[0]["attributes"]["documentation"] == documented[
        "documentation"
    ]
    documented_files = [
        node
        for node in path_document["nodes"]
        if node["kind"] == "file" and node["label"] == path_query["files"][0]["path"]
    ]
    assert len(documented_files) == 1
    assert documented_files[0]["attributes"]["documentation"] == (
        path_query["files"][0]["documentation"]
    )
    print("native GraphML/Mermaid/JSON graph projections passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
