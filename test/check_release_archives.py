"""Reject development files and local build paths in publication archives."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import PurePosixPath
import tarfile
from typing import Iterable, Iterator, Tuple
import zipfile


EXPECTED_C_SOURCE_LICENSES = {
    "pcre2",
    "tree-sitter",
    "tree-sitter-c",
    "tree-sitter-cpp",
    "tree-sitter-javascript",
    "tree-sitter-python",
    "tree-sitter-r",
    "tree-sitter-tsx",
    "tree-sitter-typescript",
    "yyjson",
}


def _members(path: str) -> Iterator[Tuple[str, bytes]]:
    if path.endswith(".whl") or path.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    yield info.filename, archive.read(info)
        return
    with tarfile.open(path, "r:*") as archive:
        for info in archive.getmembers():
            if info.isfile():
                member = archive.extractfile(info)
                if member is None:
                    raise ValueError(f"cannot read archive member: {info.name}")
                yield info.name, member.read()


def _development_path(name: str) -> bool:
    path = PurePosixPath(name)
    lowered_parts = {part.lower() for part in path.parts}
    if (
        path.is_absolute()
        or ".." in path.parts
        or ".agents" in path.parts
        or ".codex" in path.parts
        or ".git" in path.parts
        or ".gitmodules" in path.parts
        or lowered_parts.intersection({"scripts", "test", "tests"})
    ):
        return True
    basename = path.name.upper()
    return basename.endswith(".MD") and basename != "README.MD"


def check_archive(
    path: str, forbidden_prefixes: Iterable[bytes], forbidden_text: Iterable[bytes]
) -> int:
    archive_name = PurePosixPath(path).name
    if archive_name.endswith(".whl") and "-linux_" in archive_name:
        raise ValueError(f"{path}: local Linux wheel tag is not publishable")
    count = 0
    c_source_code_members: list[PurePosixPath] = []
    c_source_licenses: set[str] = set()
    c_source_manifests = 0
    c_source_manifest: bytes | None = None
    c_source_files: dict[str, bytes] = {}
    for name, data in _members(path):
        count += 1
        member_path = PurePosixPath(name)
        parts = member_path.parts
        if "csrc" in parts:
            csrc_index = parts.index("csrc")
            relative = parts[csrc_index + 1 :]
            relative_name = PurePosixPath(*relative).as_posix()
            if relative_name in c_source_files:
                raise ValueError(
                    f"{path}: duplicate C snapshot member: {relative_name}"
                )
            c_source_files[relative_name] = data
            if relative == (".archbird-manifest.json",):
                c_source_manifests += 1
                c_source_manifest = data
                c_source_code_members.append(member_path)
            elif member_path.suffix in {".c", ".h", ".sources"}:
                c_source_code_members.append(member_path)
            if (
                len(relative) == 3
                and relative[0] == "vendor"
                and relative[2] == "LICENSE"
            ):
                c_source_licenses.add(relative[1])
        if _development_path(name):
            raise ValueError(f"{path}: development or unsafe member: {name}")
        for marker in (*forbidden_prefixes, *forbidden_text):
            if marker and marker in data:
                rendered = marker.decode("utf-8", errors="backslashreplace")
                raise ValueError(
                    f"{path}: member {name} contains forbidden text: {rendered}"
                )
    if c_source_code_members:
        if c_source_manifests != 1:
            raise ValueError(
                f"{path}: expected one content-hashed C snapshot manifest, "
                f"found {c_source_manifests}"
            )
        try:
            snapshot = json.loads((c_source_manifest or b"").decode("utf-8"))
            rows = snapshot["files"]
            generated_rows = snapshot.get("generated", [])
        except (KeyError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError(f"{path}: invalid C snapshot manifest") from error
        if not isinstance(rows, list) or not isinstance(generated_rows, list):
            raise ValueError(f"{path}: invalid C snapshot file inventory")
        expected: dict[str, tuple[int, str]] = {}
        for row in (*rows, *generated_rows):
            if (
                not isinstance(row, dict)
                or not isinstance(row.get("path"), str)
                or not isinstance(row.get("bytes"), int)
                or not isinstance(row.get("sha256"), str)
            ):
                raise ValueError(f"{path}: invalid C snapshot file row")
            relative = PurePosixPath(row["path"])
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"{path}: unsafe C snapshot path: {relative}")
            name = relative.as_posix()
            if name in expected:
                raise ValueError(f"{path}: duplicate C snapshot path: {name}")
            expected[name] = (row["bytes"], row["sha256"])
        actual_names = set(c_source_files) - {".archbird-manifest.json"}
        expected_names = set(expected)
        if actual_names != expected_names:
            raise ValueError(
                f"{path}: C snapshot inventory differs: "
                f"missing={sorted(expected_names - actual_names)!r} "
                f"extra={sorted(actual_names - expected_names)!r}"
            )
        for name, (expected_size, expected_sha256) in expected.items():
            data = c_source_files[name]
            if len(data) != expected_size or hashlib.sha256(data).hexdigest() != expected_sha256:
                raise ValueError(f"{path}: C snapshot content differs: {name}")
        if c_source_licenses != EXPECTED_C_SOURCE_LICENSES:
            raise ValueError(
                f"{path}: C snapshot license inventory differs: "
                f"missing={sorted(EXPECTED_C_SOURCE_LICENSES - c_source_licenses)!r} "
                f"extra={sorted(c_source_licenses - EXPECTED_C_SOURCE_LICENSES)!r}"
            )
    return count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archives", nargs="+")
    parser.add_argument("--forbid-prefix", action="append", default=[])
    parser.add_argument("--forbid-text", action="append", default=[])
    args = parser.parse_args()
    prefixes = tuple(value.encode() for value in args.forbid_prefix if value)
    text = tuple(value.encode() for value in args.forbid_text if value)
    for path in args.archives:
        count = check_archive(path, prefixes, text)
        print(f"release archive clean: {path} ({count} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
