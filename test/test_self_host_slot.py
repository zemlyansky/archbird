"""Exercise a self-host slot after installation at its final path."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import self_host
from tools import sync_csrc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel")
    parser.add_argument("state")
    args = parser.parse_args()
    source_digest = self_host.core_source_digest(sync_csrc.REPOSITORY)
    package_digest = sync_csrc.repository_implementation_digest()
    if source_digest != package_digest:
        raise AssertionError(
            "self-host and package source closures disagree: "
            f"{source_digest} != {package_digest}"
        )
    state = Path(args.state).resolve()
    slot = self_host.install_slot(state, Path(args.wheel).resolve())
    root = self_host.slot_directory(state, slot["wheel_sha256"])
    expected = slot["version"]
    for command in (
        self_host.archbird_command(root),
        self_host.archbird_console_command(root),
    ):
        actual = subprocess.run(
            [*command, "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if actual != expected:
            raise AssertionError(f"slot command returned {actual!r}, expected {expected!r}")
    print(f"self-host slot module and console entrypoints passed: {slot['wheel_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
