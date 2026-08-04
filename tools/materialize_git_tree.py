#!/usr/bin/env python3
"""Materialize one Git tree and its pinned submodules without network access."""

from __future__ import annotations

import argparse
import configparser
import io
from pathlib import Path, PurePosixPath
import subprocess
import tarfile


def _git(repository: Path, *arguments: str, required: bool = True) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if required and result.returncode:
        raise RuntimeError(
            f"git {' '.join(arguments)} failed in {repository}: "
            f"{result.stderr.decode('utf-8', errors='replace').strip()}"
        )
    return result.stdout if result.returncode == 0 else b""


def _extract(data: bytes, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            if (
                path.is_absolute()
                or not path.parts
                or any(part in ("", ".", "..") for part in path.parts)
                or member.issym()
                or member.islnk()
                or not (member.isdir() or member.isfile())
            ):
                raise RuntimeError(
                    f"Git archive contains an unsafe member: {member.name}"
                )
        archive.extractall(destination)


def _submodules(repository: Path, revision: str) -> tuple[str, ...]:
    data = _git(
        repository,
        "show",
        f"{revision}:.gitmodules",
        required=False,
    )
    if not data:
        return ()
    parser = configparser.ConfigParser()
    parser.read_string(data.decode("utf-8"))
    paths = []
    for section in parser.sections():
        if not section.startswith("submodule "):
            raise RuntimeError(f"unexpected .gitmodules section: {section}")
        path = parser.get(section, "path", fallback="")
        pure = PurePosixPath(path)
        if (
            not path
            or pure.is_absolute()
            or any(part in ("", ".", "..") for part in pure.parts)
        ):
            raise RuntimeError(f"invalid submodule path: {path!r}")
        paths.append(path)
    return tuple(sorted(paths))


def _gitlink(repository: Path, revision: str, path: str) -> str:
    line = _git(repository, "ls-tree", revision, "--", path).decode(
        "utf-8"
    ).strip()
    fields = line.split(None, 3)
    if len(fields) != 4 or fields[0] != "160000" or fields[1] != "commit":
        raise RuntimeError(f"{revision}:{path} is not a pinned Git submodule")
    return fields[2]


def materialize(
    repository: Path,
    revision: str,
    destination: Path,
) -> None:
    if destination.exists() and any(destination.iterdir()):
        raise RuntimeError(f"destination is not empty: {destination}")
    _extract(_git(repository, "archive", "--format=tar", revision), destination)
    for path in _submodules(repository, revision):
        commit = _gitlink(repository, revision, path)
        local = repository / path
        if not (local / ".git").exists():
            raise RuntimeError(
                f"submodule {path} is not initialized; run "
                "git submodule update --init --recursive"
            )
        target = destination / path
        materialize(local, commit, target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    repository = Path(args.repository).resolve()
    output = Path(args.output).resolve()
    materialize(repository, args.revision, output)
    commit = _git(
        repository, "rev-parse", f"{args.revision}^{{commit}}"
    ).decode("ascii").strip()
    print(f"materialized Git tree: {commit} -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
