from __future__ import annotations

import json
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    repository = ROOT / "build/mcp-server-test"
    shutil.rmtree(repository, ignore_errors=True)
    (repository / "src").mkdir(parents=True)
    (repository / "src/api.py").write_text(
        "def api_open(value):\n    return value\n"
    )
    (repository / "archbird.json").write_text(
        json.dumps(
            {
                "constraints": {
                    "API-PATH": {
                        "kind": "required_paths",
                        "owner": "architecture",
                        "paths": ["src/api.py"],
                        "rationale": "The public API implementation remains mapped.",
                    }
                }
            },
            separators=(",", ":"),
        )
    )
    process = subprocess.Popen(
        [
            str(ROOT / "archbird"),
            "mcp",
            "--root",
            str(repository),
            "--no-cache",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    assert process.stderr is not None

    def request(document: dict[str, object]) -> dict[str, object]:
        process.stdin.write(
            json.dumps(document, separators=(",", ":")).encode() + b"\n"
        )
        process.stdin.flush()
        ready, _, _ = select.select([process.stdout], [], [], 30)
        if not ready:
            raise AssertionError(
                "MCP server produced no response: "
                + process.stderr.read().decode(errors="replace")
            )
        response = json.loads(process.stdout.readline())
        assert response["jsonrpc"] == "2.0"
        assert response["id"] == document["id"]
        return response

    initialized = request(
        {
            "id": 1,
            "jsonrpc": "2.0",
            "method": "initialize",
            "params": {
                "capabilities": {},
                "clientInfo": {"name": "archbird-test", "version": "1"},
                "protocolVersion": "2025-11-25",
            },
        }
    )
    assert initialized["result"]["protocolVersion"] == "2025-11-25"
    assert initialized["result"]["capabilities"]["resources"] == {
        "listChanged": False,
        "subscribe": False,
    }
    process.stdin.write(
        b'{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}\n'
    )
    process.stdin.flush()

    listed = request(
        {"id": 2, "jsonrpc": "2.0", "method": "tools/list", "params": {}}
    )
    assert [tool["name"] for tool in listed["result"]["tools"]] == [
        "archbird_status",
        "archbird_map",
        "archbird_projection",
        "archbird_query",
        "archbird_source",
        "archbird_verify",
        "archbird_diff",
    ]

    status = None
    deadline = time.monotonic() + 30
    request_id = 3
    while time.monotonic() < deadline:
        status = request(
            {
                "id": f"status-{request_id}",
                "jsonrpc": "2.0",
                "method": "tools/call",
                "params": {"arguments": {}, "name": "archbird_status"},
            }
        )["result"]["structuredContent"]
        request_id += 1
        if status["phase"] == "ready":
            break
        assert status["phase"] in {"analyzing", "waiting"}
        time.sleep(0.02)
    assert status is not None
    generation = status["generation"]
    assert status["phase"] == "ready"
    assert isinstance(generation, str) and len(generation) == 64

    mapped = request(
        {
            "id": 4,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "arguments": {
                    "detail": "compact",
                    "max_chars": 4000,
                    "view": "overview",
                },
                "name": "archbird_map",
            },
        }
    )["result"]
    assert mapped["structuredContent"]["generation"] == generation
    assert "# repository architecture evidence" in mapped["content"][0]["text"]
    map_resource = mapped["content"][1]
    assert map_resource["type"] == "resource_link"
    assert map_resource["mimeType"] == "text/markdown"

    projection_result = request(
        {
            "id": 5,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "arguments": {
                    "plan": {
                        "id": "api-path",
                        "paths": ["src/**"],
                        "select": "mapped_paths",
                    }
                },
                "name": "archbird_projection",
            },
        }
    )["result"]
    projected = projection_result["structuredContent"]
    assert projected["completeness"]["classification"] == "complete"
    assert [item["key"] for item in projected["fact"]["items"]] == ["src/api.py"]
    projection_uri = projection_result["content"][1]["uri"]

    queried = request(
        {
            "id": 6,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "arguments": {
                    "max_chars": 5000,
                    "symbols": ["api_open"],
                },
                "name": "archbird_query",
            },
        }
    )["result"]["content"][0]["text"]
    assert "api_open" in queried
    assert "src/api.py" in queried

    source_result = request(
        {
            "id": 7,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "arguments": {"path": "src/api.py"},
                "name": "archbird_source",
            },
        }
    )["result"]
    source = source_result["structuredContent"]
    assert source["text"] == "def api_open(value):\n    return value\n"
    assert source["truncated"] is False
    source_uri = source_result["content"][1]["uri"]

    verification = request(
        {
            "id": 8,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {"arguments": {}, "name": "archbird_verify"},
        }
    )["result"]["structuredContent"]
    assert verification["constraints"][0]["status"] == "pass"

    difference = request(
        {
            "id": 9,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "arguments": {"after": generation, "before": generation},
                "name": "archbird_diff",
            },
        }
    )["result"]["structuredContent"]
    assert all(
        not values
        for section in difference["sections"].values()
        for values in section.values()
    )

    resources = request(
        {"id": 10, "jsonrpc": "2.0", "method": "resources/list", "params": {}}
    )["result"]["resources"]
    resource_uris = {resource["uri"] for resource in resources}
    assert projection_uri in resource_uris
    assert source_uri in resource_uris
    assert any("/map/" in uri for uri in resource_uris)
    assert any("/verification/" in uri for uri in resource_uris)

    read_projection = request(
        {
            "id": 11,
            "jsonrpc": "2.0",
            "method": "resources/read",
            "params": {"uri": projection_uri},
        }
    )["result"]["contents"][0]
    assert read_projection["mimeType"] == "application/json"
    assert json.loads(read_projection["text"]) == projected

    read_source = request(
        {
            "id": 12,
            "jsonrpc": "2.0",
            "method": "resources/read",
            "params": {"uri": source_uri},
        }
    )["result"]["contents"][0]
    assert json.loads(read_source["text"]) == source

    rejected = request(
        {
            "id": 13,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {
                "arguments": {"path": "../outside.py"},
                "name": "archbird_source",
            },
        }
    )["result"]
    assert rejected["isError"] is True
    assert "escapes repository root" in rejected["content"][0]["text"]

    empty_query = request(
        {
            "id": 14,
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {"arguments": {}, "name": "archbird_query"},
        }
    )
    assert empty_query["error"]["code"] == -32602
    assert "selector" in empty_query["error"]["message"]

    bad_cursor = request(
        {
            "id": 15,
            "jsonrpc": "2.0",
            "method": "resources/list",
            "params": {"cursor": "unexpected"},
        }
    )
    assert bad_cursor["error"]["code"] == -32602

    process.stdin.close()
    assert process.wait(timeout=30) == 0
    assert process.stdout.read() == b""
    assert process.stderr.read() == b""
    print("MCP stdio Map, projection, Query, source, Verify, and Diff passed")


if __name__ == "__main__":
    main()
