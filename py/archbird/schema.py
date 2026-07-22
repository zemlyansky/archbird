"""Access the exact JSON schemas shipped with this Archbird version."""

from __future__ import annotations

from functools import lru_cache
from importlib import resources
import json


@lru_cache(maxsize=1)
def schema_names() -> tuple[str, ...]:
    """Return the sorted public schema filenames bundled with Archbird."""

    root = resources.files("archbird").joinpath("schemas")
    manifest = json.loads(root.joinpath(".archbird-manifest.json").read_text())
    if (
        manifest.get("artifact") != "archbird-python-schemas"
        or manifest.get("schema_version") != 1
    ):
        raise RuntimeError("invalid bundled schema manifest")
    names = tuple(row["path"] for row in manifest.get("files", ()))
    if names != tuple(sorted(set(names))) or "archbird.schema.json" not in names:
        raise RuntimeError("invalid bundled schema inventory")
    return names


def read_schema(name: str = "archbird.schema.json") -> bytes:
    """Read one bundled schema by exact filename."""

    if name not in schema_names():
        raise KeyError(name)
    return resources.files("archbird").joinpath("schemas", name).read_bytes()
