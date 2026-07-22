#!/usr/bin/env python3
"""Synchronize content-addressed public JSON schemas into package trees."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
SOURCE = REPOSITORY / "schema"
MANIFEST = ".archbird-manifest.json"
FIXED_MTIME = 946684800


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _schemas() -> dict[str, bytes]:
    result: dict[str, bytes] = {}
    for path in sorted(SOURCE.glob("*.json")):
        data = path.read_bytes()
        try:
            document = json.loads(data)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimeError(f"invalid public JSON schema: {path}") from error
        if not isinstance(document, dict) or "$schema" not in document:
            raise RuntimeError(f"public JSON schema has no $schema: {path}")
        result[path.name] = data
    if "archbird.schema.json" not in result:
        raise RuntimeError("project-configuration schema is missing")
    return result


def _manifest(artifact: str, schemas: dict[str, bytes]) -> dict[str, object]:
    return {
        "artifact": artifact,
        "schema_version": 1,
        "files": [
            {
                "bytes": len(data),
                "path": name,
                "sha256": _digest(data),
                "source": f"schema/{name}",
            }
            for name, data in schemas.items()
        ],
    }


def _load(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _current(target: Path, expected: dict[str, object]) -> bool:
    if _load(target / MANIFEST) != expected:
        return False
    expected_names = {MANIFEST}
    for row in expected["files"]:
        assert isinstance(row, dict)
        name = row["path"]
        assert isinstance(name, str)
        expected_names.add(name)
        candidate = target / name
        if (
            not candidate.is_file()
            or candidate.is_symlink()
            or candidate.stat().st_size != row["bytes"]
            or _digest(candidate.read_bytes()) != row["sha256"]
        ):
            return False
    actual_names = {
        path.relative_to(target).as_posix()
        for path in target.rglob("*")
        if path.is_file()
    }
    return actual_names == expected_names


def sync(target_name: str, *, check: bool = False) -> Path:
    targets = {
        "python": (
            REPOSITORY / "py/archbird/schemas",
            "archbird-python-schemas",
        ),
        "node": (REPOSITORY / "js/schema", "archbird-node-schemas"),
    }
    target, artifact = targets[target_name]
    schemas = _schemas()
    expected = _manifest(artifact, schemas)
    if _current(target, expected):
        return target
    if check:
        raise RuntimeError(f"schema snapshot is stale: {target}")
    if target.exists() and _load(target / MANIFEST) not in (None, expected):
        current = _load(target / MANIFEST)
        if not isinstance(current, dict) or current.get("artifact") != artifact:
            raise RuntimeError(f"refusing to replace unrecognized {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".schemas-", dir=target.parent))
    try:
        for name, data in schemas.items():
            destination = temporary / name
            destination.write_bytes(data)
            os.utime(destination, (FIXED_MTIME, FIXED_MTIME))
        manifest_data = (
            json.dumps(expected, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        )
        (temporary / MANIFEST).write_bytes(manifest_data)
        os.utime(temporary / MANIFEST, (FIXED_MTIME, FIXED_MTIME))
        backup = target.parent / f".{target.name}-previous"
        if backup.exists():
            shutil.rmtree(backup)
        if target.exists():
            os.replace(target, backup)
        os.replace(temporary, target)
        if backup.exists():
            shutil.rmtree(backup)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return target


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("targets", nargs="+", choices=("python", "node"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for target in args.targets:
        print(sync(target, check=args.check))


if __name__ == "__main__":
    main()
