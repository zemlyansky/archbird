#!/usr/bin/env python3
"""Canonicalize a Python sdist container without changing member bytes."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import gzip
import hashlib
from io import BytesIO
import os
from pathlib import Path, PurePosixPath
import tarfile
import tempfile
from typing import Optional


@dataclass(frozen=True)
class Member:
    name: str
    data: Optional[bytes]
    executable: bool


def _load(path: Path) -> tuple[Member, ...]:
    members = []
    names = set()
    roots = set()
    with tarfile.open(path, "r:gz") as archive:
        for info in archive.getmembers():
            name = info.name.rstrip("/")
            pure = PurePosixPath(name)
            if (
                not name
                or pure.is_absolute()
                or any(part in {"", ".", ".."} for part in pure.parts)
            ):
                raise ValueError(f"unsafe sdist member: {info.name!r}")
            if name in names:
                raise ValueError(f"duplicate sdist member: {name}")
            names.add(name)
            roots.add(pure.parts[0])
            if info.isdir():
                data = None
            elif info.isfile():
                source = archive.extractfile(info)
                if source is None:
                    raise ValueError(f"cannot read sdist member: {name}")
                data = source.read()
                if len(data) != info.size:
                    raise ValueError(f"truncated sdist member: {name}")
            else:
                raise ValueError(f"unsupported sdist member type: {name}")
            members.append(Member(name, data, bool(info.mode & 0o111)))
    if len(roots) != 1:
        raise ValueError("sdist must have exactly one top-level directory")
    return tuple(sorted(members, key=lambda member: member.name.encode("utf-8")))


def _write(path: Path, members: tuple[Member, ...], epoch: int) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as raw:
            with gzip.GzipFile(
                filename="",
                mode="wb",
                compresslevel=9,
                fileobj=raw,
                mtime=epoch,
            ) as compressed:
                with tarfile.open(
                    fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT
                ) as archive:
                    for member in members:
                        info = tarfile.TarInfo(member.name)
                        info.mtime = epoch
                        info.uid = 0
                        info.gid = 0
                        info.uname = ""
                        info.gname = ""
                        info.pax_headers = {}
                        if member.data is None:
                            info.type = tarfile.DIRTYPE
                            info.mode = 0o755
                            info.size = 0
                            archive.addfile(info)
                        else:
                            info.type = tarfile.REGTYPE
                            info.mode = 0o755 if member.executable else 0o644
                            info.size = len(member.data)
                            archive.addfile(info, BytesIO(member.data))
            raw.flush()
            os.fsync(raw.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def canonicalize(path: Path, epoch: int) -> str:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"sdist is not a regular file: {path}")
    if epoch < 0:
        raise ValueError("epoch must be nonnegative")
    members = _load(path)
    expected = tuple((member.name, member.data) for member in members)
    _write(path, members, epoch)
    actual = tuple((member.name, member.data) for member in _load(path))
    if actual != expected:
        raise ValueError("canonicalization changed sdist member bytes")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("--epoch", type=int, required=True)
    args = parser.parse_args()
    digest = canonicalize(args.path.resolve(), args.epoch)
    print(f"canonical sdist sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
