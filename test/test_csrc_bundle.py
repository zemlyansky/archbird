#!/usr/bin/env python3
"""Regression tests for the compressed npm C source snapshot."""

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.csrc_bundle import decode_bundle, encode_bundle


REPOSITORY = Path(__file__).resolve().parents[1]


def _run_node(package: Path, *, success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["node", "src/prepare-csrc.js"],
        cwd=package,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if (result.returncode == 0) != success:
        raise AssertionError(
            f"unexpected source expansion status {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    files = {
        ".archbird-manifest.json": b'{"artifact":"archbird-node-csrc"}\n',
        "include/example.h": b"int example(void);\n",
        "src/example.c": b'#include "example.h"\nint example(void) { return 1; }\n',
    }
    first = encode_bundle(files)
    second = encode_bundle(dict(reversed(tuple(files.items()))))
    if first != second:
        raise AssertionError("C source bundle is not deterministic")
    if decode_bundle(first) != dict(sorted(files.items())):
        raise AssertionError("C source bundle did not round-trip")

    try:
        encode_bundle({"../escape.c": b"bad"})
    except ValueError:
        pass
    else:
        raise AssertionError("C source bundle accepted path traversal")

    corrupted = bytearray(first)
    corrupted[len(corrupted) // 2] ^= 1
    try:
        decode_bundle(bytes(corrupted))
    except ValueError:
        pass
    else:
        raise AssertionError("C source bundle accepted corrupted content")

    with tempfile.TemporaryDirectory(prefix="archbird-csrc-bundle-") as raw:
        package = Path(raw)
        (package / "src").mkdir()
        shutil.copyfile(
            REPOSITORY / "js/src/prepare-csrc.js",
            package / "src/prepare-csrc.js",
        )
        (package / "csrc.snapshot.gz").write_bytes(first)
        result = _run_node(package)
        if "expanded 3 verified C source files" not in result.stderr:
            raise AssertionError("source expansion did not report verified inventory")
        for relative, expected in files.items():
            actual = (package / "csrc" / relative).read_bytes()
            if actual != expected:
                raise AssertionError(f"expanded C source differs: {relative}")
        before = hashlib.sha256(
            (package / "csrc/src/example.c").read_bytes()
        ).hexdigest()
        _run_node(package)
        after = hashlib.sha256(
            (package / "csrc/src/example.c").read_bytes()
        ).hexdigest()
        if before != after:
            raise AssertionError("repeat source preparation changed expanded content")

        (package / "csrc/src/example.c").write_bytes(b"tampered\n")
        result = _run_node(package, success=False)
        if "differs from its package snapshot" not in result.stderr:
            raise AssertionError("expanded-source mutation was not explained")

        shutil.rmtree(package / "csrc")
        (package / "csrc.snapshot.gz").write_bytes(bytes(corrupted))
        _run_node(package, success=False)
        if (package / "csrc").exists():
            raise AssertionError("failed source validation published partial content")

    print("compressed npm C source bundle passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
