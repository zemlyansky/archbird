#!/usr/bin/env python3
"""Require a Python wheel to contain exactly the current package sources."""

from __future__ import annotations

from pathlib import Path, PurePosixPath
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_python_wheel.py WHEEL PYTHON_SOURCE_ROOT")
    wheel = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve()
    expected = {
        path.relative_to(source).as_posix()
        for path in source.rglob("*.py")
        if path.is_file()
    }
    expected_app = {
        path.relative_to(source).as_posix()
        for path in (source / "app").rglob("*")
        if path.is_file()
    }
    expected_vendor_licenses = {
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
    with zipfile.ZipFile(wheel) as archive:
        members = {
            PurePosixPath(name).relative_to("archbird").as_posix()
            for name in archive.namelist()
            if name.startswith("archbird/") and name.endswith(".py")
        }
        extensions = [
            name
            for name in archive.namelist()
            if name.startswith("archbird/_native.") and name.endswith(".so")
        ]
        app_members = {
            PurePosixPath(name).relative_to("archbird").as_posix()
            for name in archive.namelist()
            if name.startswith("archbird/app/") and not name.endswith("/")
        }
        vendor_licenses = {
            PurePosixPath(name).parts[-2]
            for name in archive.namelist()
            if "/licenses/csrc/vendor/" in name and name.endswith("/LICENSE")
        }
    missing = sorted(expected - members)
    extra = sorted(members - expected)
    if missing or extra:
        raise AssertionError(
            f"wheel Python source inventory differs: missing={missing!r} extra={extra!r}"
        )
    if len(extensions) != 1:
        raise AssertionError(f"wheel has {len(extensions)} native extensions")
    if not expected_app or app_members != expected_app:
        raise AssertionError(
            "wheel visualization inventory differs: "
            f"missing={sorted(expected_app - app_members)!r} "
            f"extra={sorted(app_members - expected_app)!r}"
        )
    if vendor_licenses != expected_vendor_licenses:
        raise AssertionError(
            "wheel third-party license inventory differs: "
            f"missing={sorted(expected_vendor_licenses - vendor_licenses)!r} "
            f"extra={sorted(vendor_licenses - expected_vendor_licenses)!r}"
        )
    print(
        f"Python wheel source inventory exact: {len(expected)} modules, "
        f"app={len(app_members)} files, licenses={len(vendor_licenses)}, "
        f"extension={extensions[0]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
