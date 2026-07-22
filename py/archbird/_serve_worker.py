"""Isolated Map candidate process for the local visualization host."""

from __future__ import annotations

import base64
import json
from pathlib import Path
import sys

from .native import Project


def main() -> int:
    request = json.loads(sys.stdin.buffer.read())
    root = Path(request["root"]).resolve()
    encoded_config = request.get("config_base64")
    config = base64.b64decode(encoded_config) if encoded_config is not None else None
    if request.get("config_path") is not None:
        config = Path(request["config_path"]).read_bytes()
    if not request.get("no_config") and config is None:
        candidate = root / "archbird.json"
        if candidate.is_file() and not candidate.is_symlink():
            config = candidate.read_bytes()
    options = dict(request.get("project_options") or {})
    project = Project.from_repository(root, config=config, **options)
    map_json = project.map_json()
    document = json.loads(map_json)
    generation = document.get("evidence", {}).get("input_sha256")
    if not isinstance(generation, str) or len(generation) != 64:
        raise RuntimeError("generated Map has no valid evidence.input_sha256")
    header = {
        "files": [
            {
                "bytes": row["bytes"],
                "path": row["path"],
                "sha256": row["sha256"],
            }
            for row in document["files"]
        ],
        "generation": generation,
        "map_bytes": len(map_json),
        "project": document["project"],
    }
    sys.stdout.buffer.write(
        json.dumps(header, ensure_ascii=True, separators=(",", ":"), sort_keys=True).encode()
        + b"\n"
        + map_json
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
