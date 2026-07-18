#!/usr/bin/env python3
"""Differentially scan every supported file in configured real profiles."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

from archbird.map.analyze import glob_files
from archbird.map.config import load_config
from archbird.map.scanners import header_api_names, scan_file


SUPPORTED = {"c", "cpp", "javascript", "typescript", "vue", "r"}


def native_file_projection(row: dict) -> tuple:
    return (
        sorted(
            {
                (
                    symbol["line"],
                    symbol["name"],
                    symbol["kind"],
                    symbol["scope"],
                    symbol["signature"],
                )
                for symbol in row["symbols"]
            }
        ),
        row["call_counts"],
        row["method_call_counts"],
        set(row["imports"]),
        set(row["exports"]),
        set(row["messages"]["sends"]),
        set(row["messages"]["receives"]),
    )


def oracle_projection(facts) -> tuple:
    return (
        sorted(
            {
                (
                    symbol.line,
                    symbol.name,
                    symbol.kind,
                    symbol.scope,
                    symbol.signature,
                )
                for symbol in facts.symbols
            }
        ),
        facts.call_counts,
        facts.method_call_counts,
        facts.imports,
        facts.exports,
        facts.sends,
        facts.receives,
    )


def scan_profile(executable: Path, config_path: Path, root: Path) -> tuple[int, int]:
    config = load_config(config_path, root_override=root)
    checked = 0
    bytes_checked = 0
    for layer in config.layers:
        if layer.language not in SUPPORTED:
            continue
        public_names = header_api_names(root, layer.public_headers)
        for path in glob_files(root, layer.globs, config.exclude):
            relative = path.relative_to(root).as_posix()
            raw = path.read_bytes()
            text = raw.decode("utf-8")
            command = [str(executable), "--file-facts", layer.language, relative]
            if layer.language in {"c", "cpp"}:
                command.extend(sorted(public_names))
            completed = subprocess.run(
                command, input=raw, capture_output=True, check=True
            )
            artifact = json.loads(completed.stdout)
            subject = next(row for row in artifact["files"] if row["path"] == relative)
            native = native_file_projection(subject)
            oracle = scan_file(
                relative,
                layer.name,
                layer.language,
                text,
                hashlib.sha256(raw).hexdigest(),
                public_names,
            )
            wanted = oracle_projection(oracle)
            if native != wanted:
                raise AssertionError(
                    f"{config.project}:{relative}: native projection differs\n"
                    f"native={native!r}\noracle={wanted!r}"
                )
            checked += 1
            bytes_checked += len(raw)
    return checked, bytes_checked


def main() -> int:
    if len(sys.argv) < 4 or (len(sys.argv) - 2) % 2:
        raise SystemExit(
            "usage: test_scanner_profiles.py SCANNER_CLI CONFIG ROOT [CONFIG ROOT ...]"
        )
    executable = Path(sys.argv[1]).resolve()
    total_files = 0
    total_bytes = 0
    profiles = 0
    for index in range(2, len(sys.argv), 2):
        files, byte_count = scan_profile(
            executable, Path(sys.argv[index]).resolve(), Path(sys.argv[index + 1]).resolve()
        )
        total_files += files
        total_bytes += byte_count
        profiles += 1
    print(
        f"native scanner profile parity passed: {profiles} profiles, "
        f"{total_files} files, {total_bytes} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
