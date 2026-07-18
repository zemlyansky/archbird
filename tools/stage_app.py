#!/usr/bin/env python3
"""Stage or clean the generated visualization in one release package."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil


REPOSITORY = Path(__file__).resolve().parents[1]
MARKER = ".archbird-app-meta.json"
ARTIFACT = "archbird-visualization-application"


def _destination(frontend: str) -> Path:
    return (
        REPOSITORY / "py" / "archbird" / "app"
        if frontend == "python"
        else REPOSITORY / "js" / "app"
    )


def _validate_source(source: Path) -> list[dict[str, object]]:
    if not (source / "index.html").is_file() or not (source / "archbird.wasm").is_file():
        raise RuntimeError("app/dist must contain index.html and archbird.wasm")
    rows = []
    for path in sorted(source.rglob("*"), key=lambda item: item.relative_to(source).as_posix()):
        if path.is_symlink():
            raise RuntimeError(f"visualization output contains a symlink: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(source).as_posix()
        data = path.read_bytes()
        rows.append({
            "bytes": len(data),
            "path": relative,
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    return rows


def _known(destination: Path) -> bool:
    try:
        document = json.loads((destination / MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return document.get("artifact") == ARTIFACT and document.get("schema_version") == 1


def clean(frontend: str) -> None:
    destination = _destination(frontend)
    if not destination.exists():
        return
    if not _known(destination):
        raise RuntimeError(f"refusing to remove unrecognized staged app: {destination}")
    shutil.rmtree(destination)


def stage(frontend: str, source: Path) -> None:
    destination = _destination(frontend)
    rows = _validate_source(source)
    if destination.exists():
        clean(frontend)
    shutil.copytree(source, destination, copy_function=shutil.copyfile)
    (destination / MARKER).write_text(
        json.dumps(
            {"artifact": ARTIFACT, "files": rows, "schema_version": 1},
            ensure_ascii=True,
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    print(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frontend", choices=("node", "python"))
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--source", default=str(REPOSITORY / "app" / "dist"))
    args = parser.parse_args()
    if args.clean:
        clean(args.frontend)
    else:
        stage(args.frontend, Path(args.source).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
