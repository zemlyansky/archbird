#!/usr/bin/env python3
"""Check native provider merge -> compatibility file-row reduction."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


SOURCE = b"""
function run() { return dep.call() }
module.exports.run = run
exports.extra = run
postMessage({type: 'done'})
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_file_facts.py SCANNER_CLI")
    executable = Path(sys.argv[1]).resolve()
    command = [
        str(executable),
        "--file-facts",
        "javascript",
        "src/example.js",
    ]
    first = subprocess.run(command, input=SOURCE, capture_output=True, check=True)
    second = subprocess.run(command, input=SOURCE, capture_output=True, check=True)
    if first.stdout != second.stdout:
        raise AssertionError("file-facts bytes changed across identical runs")
    data = json.loads(first.stdout)
    if data["artifact"] != "archbird-file-facts" or data["schema_version"] != 1:
        raise AssertionError("unexpected file-facts identity")
    if len(data["files"]) != 1:
        raise AssertionError("unexpected file count")
    row = data["files"][0]
    if row["exports"] != ["extra", "run"]:
        raise AssertionError(f"unexpected exports: {row['exports']!r}")
    if row["method_call_counts"] != {"call": 1}:
        raise AssertionError(f"unexpected method calls: {row['method_call_counts']!r}")
    if row["messages"] != {"receives": [], "sends": ["type:done"]}:
        raise AssertionError(f"unexpected messages: {row['messages']!r}")
    if [symbol["name"] for symbol in row["symbols"]] != ["run"]:
        raise AssertionError(f"unexpected symbols: {row['symbols']!r}")
    print("native compatibility file facts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
