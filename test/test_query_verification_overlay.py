#!/usr/bin/env python3
"""Shared change-brief Verification overlay contract and CLI parity."""

from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from archbird import _native
from archbird.native import (
    Project,
    evaluate_constraints_json,
    query_map_json,
    query_map_markdown,
)


def seal_verification(document: dict) -> bytes:
    payload = {
        key: value
        for key, value in document.items()
        if key != "verification_result_sha256"
    }
    document["verification_result_sha256"] = hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return json.dumps(
        document, sort_keys=True, separators=(",", ":")
    ).encode()


def run(arguments: list[str], cwd: Path, env: dict[str, str]) -> bytes:
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise AssertionError(
            f"command failed: {arguments!r}\n"
            f"stdout={completed.stdout.decode(errors='replace')}\n"
            f"stderr={completed.stderr.decode(errors='replace')}"
        )
    return completed.stdout


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_query_verification_overlay.py REPOSITORY NODE ADDON")
    repository = Path(sys.argv[1]).resolve()
    node = sys.argv[2]
    addon = Path(sys.argv[3]).resolve()
    fixture = repository / "test/fixtures/map_base"
    config_bytes = (fixture / "archbird.json").read_bytes()
    project = Project.from_config(fixture / "archbird.json", root=fixture)
    map_bytes = project.map_json()
    query = json.loads(query_map_json(map_bytes, paths=["js/index.js"]))
    result = json.loads(evaluate_constraints_json(config_bytes, map_bytes))
    result_bytes = seal_verification(result)

    markdown = query_map_markdown(
        map_bytes,
        paths=["js/index.js"],
        view="changes",
        verification_result=result_bytes,
    ).decode()
    for wanted in (
        "## Architecture constraints",
        "Verification `map-base`; evidence=current; producer=current; relevant=1/3 constraints; findings=1",
        "fail error JAVASCRIPT-ENTRY owner=architecture requirements=ARCH-JS-001 findings=1 paths=js/index.js",
        "extra twice [open,current,applicable]: unexpected actual fact twice @ js/index.js:5",
    ):
        if wanted not in markdown:
            raise AssertionError(f"overlay omitted {wanted!r}\n{markdown}")
    if "OTHER-ENTRY" in markdown or "PYTHON-ENTRY" in markdown:
        raise AssertionError("overlay leaked a constraint from an unrelated path")

    stale = copy.deepcopy(result)
    current = next(row for row in stale["evaluations"] if row["id"] == "current")
    current["map_input_sha256"] = "3" * 64
    stale_markdown = query_map_markdown(
        map_bytes,
        paths=["js/index.js"],
        view="changes",
        verification_result=seal_verification(stale),
    ).decode()
    if "evidence=stale" not in stale_markdown:
        raise AssertionError("overlay hid stale verification inputs")

    ambiguous = copy.deepcopy(stale)
    ambiguous["evaluations"].append(copy.deepcopy(current))
    try:
        query_map_markdown(
            map_bytes,
            paths=["js/index.js"],
            view="changes",
            verification_result=seal_verification(ambiguous),
        )
    except (_native.Error, ValueError):
        pass
    else:
        raise AssertionError("overlay accepted duplicate current Map evaluations")

    malformed = copy.deepcopy(result)
    del malformed["evaluations"][0]["map_input_sha256"]
    try:
        query_map_markdown(
            map_bytes,
            paths=["js/index.js"],
            view="changes",
            verification_result=seal_verification(malformed),
        )
    except (_native.Error, ValueError):
        pass
    else:
        raise AssertionError("overlay accepted a malformed unrelated project row")

    try:
        query_map_markdown(
            map_bytes,
            paths=["js/index.js"],
            view="focused",
            verification_result=result_bytes,
        )
    except ValueError:
        pass
    else:
        raise AssertionError("focused view accepted a Verification overlay")

    map_path = repository / "build/tmp/query-overlay.map.json"
    map_path.parent.mkdir(parents=True, exist_ok=True)
    map_path.write_bytes(map_bytes)
    temporary = repository / "build/tmp/query-overlay.result.json"
    temporary.parent.mkdir(parents=True, exist_ok=True)
    temporary.write_bytes(result_bytes)
    common = [
        "query",
        "--map",
        str(map_path),
        "--path",
        "js/index.js",
        "--view",
        "changes",
        "--verification-result",
        str(temporary),
        "--progress",
        "never",
    ]
    python_env = os.environ.copy()
    python_env["PYTHONPATH"] = str(repository / "py")
    python_output = run([sys.executable, "-m", "archbird", *common], repository, python_env)
    node_env = os.environ.copy()
    node_env["ARCHBIRD_ENGINE"] = "native"
    node_env["ARCHBIRD_NATIVE_ADDON"] = str(addon)
    node_output = run([node, str(repository / "js/src/cli.js"), *common], repository, node_env)
    if python_output != node_output or python_output != markdown.encode():
        raise AssertionError("Python/Node/API Verification overlays diverged")
    temporary.unlink()
    map_path.unlink()
    print("change-brief Verification overlay parity passed")


if __name__ == "__main__":
    main()
