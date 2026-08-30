#!/usr/bin/env python3
"""Validate the portable consumer skill against the live Archbird CLI."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / "skills/archbird/SKILL.md"
FIXTURE = ROOT / "test/fixtures/map_base"


def run(
    *arguments: str, check: bool = True
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["PYTHON"] = sys.executable
    environment["PYTHONPATH"] = str(ROOT / "py")
    return subprocess.run(
        [str(ROOT / "archbird"), *arguments],
        check=check,
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def frontmatter(text: str) -> str:
    if not text.startswith("---\n"):
        raise AssertionError("SKILL.md must start with YAML frontmatter")
    parts = text.split("---\n", 2)
    if len(parts) != 3:
        raise AssertionError("SKILL.md frontmatter is not closed")
    return parts[1]


def project_version() -> str:
    text = (ROOT / "py/pyproject.toml").read_text(encoding="utf-8")
    match = re.search(r'^version = "([^"]+)"$', text, re.MULTILINE)
    if match is None:
        raise AssertionError("Python project version is absent")
    return match.group(1)


def check_skill_contract() -> None:
    text = SKILL.read_text(encoding="utf-8")
    metadata = frontmatter(text)
    assert re.search(r"^name: archbird$", metadata, re.MULTILINE)
    assert re.search(r"^description: \S", metadata, re.MULTILINE)
    assert re.search(r"^license: Apache-2.0$", metadata, re.MULTILINE)
    assert f'version: "{project_version()}"' in metadata
    assert len(text.splitlines()) <= 200

    forbidden = (
        "/home/anton",
        "build/source-python",
        "FEEDBACK.md",
        "make build",
        "make test",
    )
    for value in forbidden:
        assert value not in text, f"consumer skill contains maintainer detail {value!r}"

    for command in (
        "archbird map",
        "archbird query",
        "archbird path",
        "archbird freshness",
        "archbird verify",
        "archbird diff",
        "archbird plan",
        "archbird act",
        "archbird apply",
        "archbird mcp",
    ):
        assert command in text, f"consumer workflow omits {command}"

    for phrase in (
        "candidate",
        "conservative",
        "measurements, not quality scores",
        "observed",
        "smallest",
        "aggregator",
        "source directly",
    ):
        assert phrase in text, f"consumer skill omits boundary {phrase!r}"

    for command in (
        "archbird map . --view metrics",
        "archbird query . --symbol 'PATH:SYMBOL' --view metrics --detail full",
    ):
        assert command in text, f"consumer skill omits metric workflow {command!r}"


def check_public_installation() -> None:
    install = "npx skills add zemlyansky/archbird --skill archbird -g"
    for relative in ("README.md", "py/README.md", "js/README.md"):
        text = (ROOT / relative).read_text(encoding="utf-8")
        assert install in text, f"{relative} omits portable skill installation"
        assert "Copy this compact policy" not in text, (
            f"{relative} retains a copied agent policy that can drift"
        )


def check_cli_contract() -> None:
    root = str(FIXTURE)
    commands = (
        (
            0,
            "map",
            root,
            "--view",
            "overview",
            "--detail",
            "compact",
            "--max-chars",
            "12000",
            "--check",
        ),
        (
            0,
            "query",
            root,
            "--symbol",
            "js/index.js:add",
            "--depth",
            "1",
            "--test-depth",
            "1",
            "--max-chars",
            "12000",
            "--check",
        ),
        (
            0,
            "query",
            root,
            "--symbol",
            "js/index.js:add",
            "--view",
            "source",
            "--detail",
            "standard",
            "--max-chars",
            "12000",
            "--check",
        ),
        (0, "query", root, "--path", "js/index.js", "--dump", "--check"),
        (
            0,
            "path",
            "py/pkg/__init__.py",
            "py/pkg/api.py",
            "--root",
            root,
            "--check",
        ),
        (1, "verify", "JAVASCRIPT-ENTRY", "--root", root, "--check"),
        (0, "diff", root, root, "--check"),
    )
    for expected_status, *command in commands:
        result = run(*command, check=False)
        assert result.returncode == expected_status, result.stderr
        assert result.stdout.strip(), f"{' '.join(command)} emitted no result"

    with tempfile.TemporaryDirectory(prefix="archbird-skill-") as directory:
        snapshot = Path(directory) / "map.json"
        run("map", root, "--format", "json", "--output", str(snapshot), "--check")
        result = run(
            "freshness",
            "--root",
            root,
            "--snapshot",
            str(snapshot),
            "--check",
        )
        assert result.stdout.strip()


def main() -> int:
    check_skill_contract()
    check_public_installation()
    check_cli_contract()
    print("consumer agent skill contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
