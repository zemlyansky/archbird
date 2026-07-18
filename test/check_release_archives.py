"""Reject development files and local build paths in publication archives."""

from __future__ import annotations

import argparse
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
    for name, data in _members(path):
        count += 1
        member_path = PurePosixPath(name)
        parts = member_path.parts
        if "csrc" in parts:
            csrc_index = parts.index("csrc")
            relative = parts[csrc_index + 1 :]
            if relative == (".archbird-manifest.json",):
                c_source_manifests += 1
                c_source_code_members.append(member_path)
            elif member_path.suffix in {".c", ".h"}:
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
