#!/usr/bin/env python3
"""Validate GitHub Actions workflows with a pinned actionlint binary."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
from typing import Optional
import urllib.error
import urllib.request
import zipfile


VERSION = "1.7.12"
RELEASE_BASE = f"https://github.com/rhysd/actionlint/releases/download/v{VERSION}"
ARCHIVES = {
    ("linux", "x86_64"): (
        f"actionlint_{VERSION}_linux_amd64.tar.gz",
        "8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8",
    ),
    ("linux", "aarch64"): (
        f"actionlint_{VERSION}_linux_arm64.tar.gz",
        "325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6",
    ),
    ("darwin", "x86_64"): (
        f"actionlint_{VERSION}_darwin_amd64.tar.gz",
        "5b44c3bc2255115c9b69e30efc0fecdf498fdb63c5d58e17084fd5f16324c644",
    ),
    ("darwin", "arm64"): (
        f"actionlint_{VERSION}_darwin_arm64.tar.gz",
        "aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f",
    ),
    ("windows", "amd64"): (
        f"actionlint_{VERSION}_windows_amd64.zip",
        "6e7241b51e6817ea6a047693d8e6fed13b31819c9a0dd6c5a726e1592d22f6e9",
    ),
    ("windows", "arm64"): (
        f"actionlint_{VERSION}_windows_arm64.zip",
        "cadcf7ea4efe3a68728893813643cebe1185e5b1d4be5b96245f65c9a4d5ea41",
    ),
}


def _platform_key() -> tuple[str, str]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    aliases = {"amd64": "x86_64", "arm64": "aarch64"}
    machine = aliases.get(machine, machine)
    if system == "windows":
        machine = {"x86_64": "amd64", "aarch64": "arm64"}.get(machine, machine)
    elif system == "darwin" and machine == "aarch64":
        machine = "arm64"
    return system, machine


def _download_binary(cache_dir: Path) -> Path:
    key = _platform_key()
    archive = ARCHIVES.get(key)
    if archive is None:
        supported = ", ".join(f"{system}/{machine}" for system, machine in ARCHIVES)
        raise RuntimeError(
            f"actionlint {VERSION} has no pinned binary for {key[0]}/{key[1]}; "
            f"supported: {supported}; pass --actionlint PATH"
        )
    archive_name, expected_sha256 = archive
    executable_name = "actionlint.exe" if key[0] == "windows" else "actionlint"
    destination = cache_dir / f"actionlint-{VERSION}-{key[0]}-{key[1]}" / executable_name
    if destination.is_file():
        return destination

    request = urllib.request.Request(
        f"{RELEASE_BASE}/{archive_name}",
        headers={"User-Agent": "archbird-workflow-check"},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = response.read()
    actual_sha256 = hashlib.sha256(payload).hexdigest()
    if actual_sha256 != expected_sha256:
        raise RuntimeError(
            f"actionlint archive digest mismatch: expected {expected_sha256}, got {actual_sha256}"
        )

    if archive_name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(payload)) as archive_file:
            binary = archive_file.read(executable_name)
    else:
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive_file:
            member = archive_file.getmember("actionlint")
            extracted = archive_file.extractfile(member)
            if extracted is None:
                raise RuntimeError("actionlint archive does not contain its executable")
            binary = extracted.read()

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
    temporary.write_bytes(binary)
    temporary.chmod(temporary.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    os.replace(temporary, destination)
    return destination


def _resolve_binary(explicit: Optional[str], cache_dir: Path) -> Path:
    if explicit:
        resolved = shutil.which(explicit) if os.sep not in explicit else explicit
        if not resolved or not Path(resolved).is_file():
            raise RuntimeError(f"actionlint executable not found: {explicit}")
        return Path(resolved)
    return _download_binary(cache_dir)


def _run(binary: Path, files: list[Path]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "-color", *[str(path) for path in files]],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def _mutation_test(binary: Path, scratch_dir: Path) -> None:
    scratch_dir.mkdir(parents=True, exist_ok=True)
    valid = scratch_dir / "valid.yml"
    invalid = scratch_dir / "invalid.yml"
    valid.write_text(
        "name: valid\n"
        "on: push\n"
        "jobs:\n"
        "  test:\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - run: echo ok\n"
        "        env:\n"
        "          CACHE: ${{ runner.temp }}/cache\n",
        encoding="utf-8",
    )
    invalid.write_text(
        "name: invalid\n"
        "on: push\n"
        "jobs:\n"
        "  test:\n"
        "    runs-on: ubuntu-latest\n"
        "    env:\n"
        "      CACHE: ${{ runner.temp }}/cache\n"
        "    steps:\n"
        "      - run: echo bad\n",
        encoding="utf-8",
    )
    valid_result = _run(binary, [valid])
    if valid_result.returncode != 0:
        raise RuntimeError(f"actionlint rejected the valid context fixture:\n{valid_result.stdout}")
    invalid_result = _run(binary, [invalid])
    if invalid_result.returncode == 0 or "runner" not in invalid_result.stdout:
        raise RuntimeError(
            "actionlint did not reject the job-level runner-context mutation:\n"
            f"{invalid_result.stdout}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--scratch-dir", type=Path, required=True)
    parser.add_argument("--actionlint")
    args = parser.parse_args()

    root = args.root.resolve()
    workflows = sorted((root / ".github" / "workflows").glob("*.y*ml"))
    if not workflows:
        raise RuntimeError(f"no GitHub Actions workflows found below {root}")
    binary = _resolve_binary(args.actionlint, args.cache_dir.resolve())
    result = _run(binary, workflows)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        return result.returncode
    _mutation_test(binary, args.scratch_dir.resolve())
    print(f"GitHub workflows pass actionlint {VERSION}; invalid-context mutation rejected")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, urllib.error.URLError) as error:
        print(f"workflow check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
