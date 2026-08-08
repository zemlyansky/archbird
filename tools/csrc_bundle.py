#!/usr/bin/env python3
"""Pack and validate the npm C source snapshot as one deterministic gzip file."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
from typing import Mapping


MAGIC = b"ARCHBIRD_CSRC_BUNDLE_V1\n"
ARTIFACT = "archbird-c-source-bundle"


def _safe_path(value: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or ".." in path.parts
        or "." in path.parts
        or "\\" in value
    ):
        raise ValueError(f"unsafe C snapshot path: {value!r}")
    return path.as_posix()


def encode_bundle(files: Mapping[str, bytes]) -> bytes:
    rows: list[dict[str, object]] = []
    content = bytearray()
    for raw_path in sorted(files):
        path = _safe_path(raw_path)
        data = files[raw_path]
        if not isinstance(data, bytes):
            raise TypeError(f"C snapshot member is not bytes: {path}")
        rows.append(
            {
                "bytes": len(data),
                "offset": len(content),
                "path": path,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
        content.extend(data)
    header = json.dumps(
        {"artifact": ARTIFACT, "files": rows},
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    payload = MAGIC + struct.pack(">Q", len(header)) + header + content
    return gzip.compress(payload, compresslevel=9, mtime=0)


def decode_bundle(encoded: bytes) -> dict[str, bytes]:
    try:
        payload = gzip.decompress(encoded)
    except (EOFError, OSError) as error:
        raise ValueError("invalid compressed C source bundle") from error
    prefix = len(MAGIC) + 8
    if len(payload) < prefix or not payload.startswith(MAGIC):
        raise ValueError("invalid C source bundle signature")
    header_length = struct.unpack(">Q", payload[len(MAGIC) : prefix])[0]
    header_end = prefix + header_length
    if header_end > len(payload):
        raise ValueError("truncated C source bundle header")
    try:
        header = json.loads(payload[prefix:header_end])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("invalid C source bundle header") from error
    if not isinstance(header, dict) or header.get("artifact") != ARTIFACT:
        raise ValueError("unknown C source bundle artifact")
    rows = header.get("files")
    if not isinstance(rows, list):
        raise ValueError("invalid C source bundle inventory")
    content = payload[header_end:]
    result: dict[str, bytes] = {}
    expected_offset = 0
    for row in rows:
        if (
            not isinstance(row, dict)
            or not isinstance(row.get("path"), str)
            or not isinstance(row.get("offset"), int)
            or not isinstance(row.get("bytes"), int)
            or not isinstance(row.get("sha256"), str)
        ):
            raise ValueError("invalid C source bundle row")
        path = _safe_path(row["path"])
        offset = row["offset"]
        length = row["bytes"]
        if path in result or offset != expected_offset or length < 0:
            raise ValueError("invalid C source bundle ordering")
        end = offset + length
        if end > len(content):
            raise ValueError(f"truncated C source bundle member: {path}")
        data = bytes(content[offset:end])
        if hashlib.sha256(data).hexdigest() != row["sha256"]:
            raise ValueError(f"C source bundle member digest differs: {path}")
        result[path] = data
        expected_offset = end
    if expected_offset != len(content):
        raise ValueError("C source bundle has trailing content")
    return result


def pack_directory(source: Path, destination: Path) -> None:
    files = {
        path.relative_to(source).as_posix(): path.read_bytes()
        for path in source.rglob("*")
        if path.is_file()
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(encode_bundle(files))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    if not args.source.is_dir():
        parser.error(f"source is not a directory: {args.source}")
    pack_directory(args.source, args.destination)
    decoded = decode_bundle(args.destination.read_bytes())
    print(
        f"packed {len(decoded)} C source files into {args.destination} "
        f"({args.destination.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
