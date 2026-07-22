#!/usr/bin/env python3
"""Prove that a source-checkout frontend loaded the current shared C core."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from sync_csrc import REPOSITORY, repository_implementation_digest


CONFIG = {
    "layers": [
        {
            "globs": ["identity.c"],
            "language": "c",
            "name": "core",
            "role": "core",
        }
    ],
    "project": "source-coherence",
    "schema_version": 2,
}
SOURCE = b"int archbird_source_identity(void) { return 1; }\n"


def _python_digest() -> str:
    from archbird import Project, Source

    project = Project(
        "source-coherence",
        [Source("identity.c", SOURCE, language="c", layer="core")],
    )
    project.set_config(
        json.dumps(CONFIG, separators=(",", ":"), sort_keys=True).encode()
    )
    project.scan()
    return str(json.loads(project.map_json())["tool"]["implementation_sha256"])


def _node_digest(node: str, addon: Path) -> str:
    script = r"""
const archbird = require(process.env.ARCHBIRD_SOURCE_PACKAGE);
const config = {
  layers: [{globs: ["identity.c"], language: "c", name: "core", role: "core"}],
  project: "source-coherence",
  schema_version: 2,
};
const project = new archbird.Project("source-coherence", [
  new archbird.Source(
    "identity.c",
    Buffer.from("int archbird_source_identity(void) { return 1; }\n"),
    {language: "c", layer: "core"},
  ),
]);
try {
  project.setConfig(Buffer.from(JSON.stringify(config)));
  project.scan("primary", {typescript: false});
  process.stdout.write(JSON.parse(project.mapJson()).tool.implementation_sha256);
} finally {
  project.dispose();
}
"""
    environment = os.environ.copy()
    environment.update(
        {
            "ARCHBIRD_ENGINE": "native",
            "ARCHBIRD_NATIVE_ADDON": str(addon.resolve()),
            "ARCHBIRD_SOURCE_PACKAGE": str((REPOSITORY / "js").resolve()),
        }
    )
    completed = subprocess.run(
        [node, "-e", script],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    return completed.stdout.decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frontend", choices=("python", "node"))
    parser.add_argument("--node", default="node")
    parser.add_argument(
        "--addon", default=str(REPOSITORY / "js/build/Release/_native.node")
    )
    args = parser.parse_args()

    expected = repository_implementation_digest()
    actual = (
        _python_digest()
        if args.frontend == "python"
        else _node_digest(args.node, Path(args.addon))
    )
    if re.fullmatch(r"[0-9a-f]{64}", actual) is None:
        raise SystemExit(f"{args.frontend} frontend emitted an invalid core digest")
    if actual != expected:
        raise SystemExit(
            f"{args.frontend} frontend loaded stale core {actual}; expected {expected}"
        )
    print(f"{args.frontend} frontend loaded current core {actual}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
