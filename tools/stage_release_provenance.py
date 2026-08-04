#!/usr/bin/env python3
"""Stage or clean exact source provenance for publishable Archbird packages."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


REPOSITORY = Path(__file__).resolve().parents[1]
STATE = REPOSITORY / "build/release-tmp/node-release-provenance-state.json"
PYTHON_PROVENANCE = REPOSITORY / "py/archbird/release.json"
NODE_PROVENANCE = REPOSITORY / "js/release.json"
NODE_PACKAGE = REPOSITORY / "js/package.json"
ARTIFACT = "archbird-release-source"
STATE_ARTIFACT = "archbird-node-release-provenance-state"


def _git(*arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(REPOSITORY), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"git {' '.join(arguments)} failed: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def _version() -> str:
    text = (REPOSITORY / "py/pyproject.toml").read_text(encoding="utf-8")
    match = re.search(r'^version = "([^"]+)"$', text, re.MULTILINE)
    if match is None:
        raise RuntimeError("py/pyproject.toml has no project version")
    version = match.group(1)
    node = json.loads(NODE_PACKAGE.read_text(encoding="utf-8")).get("version")
    if node != version:
        raise RuntimeError(
            f"Python and npm release versions differ: {version!r} != {node!r}"
        )
    return version


def release_document() -> dict[str, object]:
    dirty = _git("status", "--porcelain", "--untracked-files=no")
    if dirty:
        raise RuntimeError(
            "release provenance requires a clean tracked worktree:\n" + dirty
        )
    untracked = [
        path
        for path in _git(
            "ls-files", "--others", "--exclude-standard"
        ).splitlines()
        if path != ".claude" and not path.startswith(".claude/")
    ]
    if untracked:
        raise RuntimeError(
            "release provenance requires all source files to belong to the "
            "tagged tree; unexpected untracked paths:\n"
            + "\n".join(untracked)
        )
    commit = _git("rev-parse", "HEAD^{commit}")
    tree = _git("rev-parse", "HEAD^{tree}")
    version = _version()
    tag = f"v{version}"
    try:
        tagged = _git("rev-parse", f"{tag}^{{commit}}")
    except RuntimeError as error:
        raise RuntimeError(
            f"release tag {tag} must exist before building archives"
        ) from error
    if tagged != commit:
        raise RuntimeError(
            f"release tag {tag} points to {tagged}, not source commit {commit}"
        )
    return {
        "artifact": ARTIFACT,
        "schema_version": 1,
        "source_commit": commit,
        "source_tree": tree,
        "tag": tag,
        "version": version,
    }


def _encoded(document: dict[str, object]) -> bytes:
    return (
        json.dumps(document, ensure_ascii=True, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def _known_provenance(path: Path) -> bool:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return document.get("artifact") == ARTIFACT


def stage_python(document: dict[str, object]) -> None:
    if PYTHON_PROVENANCE.exists() and not _known_provenance(PYTHON_PROVENANCE):
        raise RuntimeError(
            f"refusing to replace unrecognized provenance: {PYTHON_PROVENANCE}"
        )
    PYTHON_PROVENANCE.write_bytes(_encoded(document))
    print(PYTHON_PROVENANCE)


def clean_python() -> None:
    if not PYTHON_PROVENANCE.exists():
        return
    if not _known_provenance(PYTHON_PROVENANCE):
        raise RuntimeError(
            f"refusing to remove unrecognized provenance: {PYTHON_PROVENANCE}"
        )
    PYTHON_PROVENANCE.unlink()


def stage_node(document: dict[str, object]) -> None:
    if STATE.exists():
        raise RuntimeError(
            f"unfinished Node release staging exists; clean it first: {STATE}"
        )
    if NODE_PROVENANCE.exists() and not _known_provenance(NODE_PROVENANCE):
        raise RuntimeError(
            f"refusing to replace unrecognized provenance: {NODE_PROVENANCE}"
        )
    original = NODE_PACKAGE.read_bytes()
    package = json.loads(original)
    if "gitHead" in package:
        raise RuntimeError("tracked js/package.json must not contain gitHead")
    package["gitHead"] = document["source_commit"]
    staged = (
        json.dumps(package, ensure_ascii=False, indent=2) + "\n"
    ).encode("utf-8")
    state = {
        "artifact": STATE_ARTIFACT,
        "original_package": original.decode("utf-8"),
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "staged_sha256": hashlib.sha256(staged).hexdigest(),
    }
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_bytes(_encoded(state))
    NODE_PROVENANCE.write_bytes(_encoded(document))
    NODE_PACKAGE.write_bytes(staged)
    print(NODE_PROVENANCE)


def clean_node() -> None:
    if not STATE.exists():
        if NODE_PROVENANCE.exists():
            if not _known_provenance(NODE_PROVENANCE):
                raise RuntimeError(
                    f"refusing to remove unrecognized provenance: {NODE_PROVENANCE}"
                )
            NODE_PROVENANCE.unlink()
        return
    try:
        state = json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError(f"invalid Node release staging state: {STATE}") from error
    if state.get("artifact") != STATE_ARTIFACT:
        raise RuntimeError(f"unrecognized Node release staging state: {STATE}")
    current = NODE_PACKAGE.read_bytes()
    original = state.get("original_package")
    if not isinstance(original, str):
        raise RuntimeError("Node release staging state omitted original package")
    original_bytes = original.encode("utf-8")
    if hashlib.sha256(original_bytes).hexdigest() != state.get(
        "original_sha256"
    ):
        raise RuntimeError("Node release staging original package digest differs")
    current_sha256 = hashlib.sha256(current).hexdigest()
    if current_sha256 == state.get("staged_sha256"):
        NODE_PACKAGE.write_bytes(original_bytes)
    elif current_sha256 != state.get("original_sha256"):
        raise RuntimeError(
            "refusing to overwrite js/package.json changed during release staging"
        )
    if NODE_PROVENANCE.exists():
        if not _known_provenance(NODE_PROVENANCE):
            raise RuntimeError(
                f"refusing to remove unrecognized provenance: {NODE_PROVENANCE}"
            )
        NODE_PROVENANCE.unlink()
    STATE.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("check", "python", "node"))
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()
    if args.target == "check":
        if args.clean:
            raise SystemExit("check does not accept --clean")
        document = release_document()
        print(
            f"release source passed: {document['tag']} "
            f"{document['source_commit']}"
        )
        return 0
    if args.clean:
        (clean_python if args.target == "python" else clean_node)()
        return 0
    document = release_document()
    (stage_python if args.target == "python" else stage_node)(document)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
