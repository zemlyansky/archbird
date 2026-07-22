#!/usr/bin/env python3
"""Cross-frontend Git change-set adapter and canonical Query contract."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from archbird.native import query_map_json


def run(arguments: list[str], *, cwd: Path, env: dict[str, str] | None = None):
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise AssertionError(
            f"command failed ({completed.returncode}): {arguments!r}\n"
            f"stdout={completed.stdout.decode(errors='replace')}\n"
            f"stderr={completed.stderr.decode(errors='replace')}"
        )
    return completed


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_git_diff_cli.py REPOSITORY NODE ADDON")
    repository = Path(sys.argv[1]).resolve()
    node = sys.argv[2]
    addon = Path(sys.argv[3]).resolve()
    temporary_root = repository / "build" / "tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as raw:
        root = Path(raw)
        write(root / "package.json", '{"main":"src/index.js","name":"git-change"}\n')
        write(root / "src/index.js", "export function current() { return 1; }\n")
        write(root / "src/old.js", "export const oldName = 1;\n")
        write(root / "README.md", "# before\n")
        run(["git", "init", "-q"], cwd=root)
        run(["git", "add", "."], cwd=root)
        run(
            [
                "git",
                "-c",
                "user.name=Archbird Test",
                "-c",
                "user.email=archbird@example.invalid",
                "commit",
                "-qm",
                "fixture",
            ],
            cwd=root,
        )
        write(root / "src/index.js", "export function current() { return 2; }\n")
        run(["git", "mv", "src/old.js", "src/new.js"], cwd=root)
        (root / "README.md").unlink()
        write(root / "src/added.js", "export const added = true;\n")
        run(["git", "add", "src/added.js"], cwd=root)

        python_output = root / "python-query.json"
        node_output = root / "node-query.json"
        map_output = root / "map.json"
        python_env = os.environ.copy()
        python_env["PYTHONPATH"] = str(repository / "py")
        run(
            [
                sys.executable,
                "-m",
                "archbird",
                str(root),
                "--no-config",
                "--format",
                "json",
                "--no-cache",
                "--progress",
                "never",
                "--output",
                str(map_output),
                "--check",
            ],
            cwd=repository,
            env=python_env,
        )
        common = [
            "query",
            "--root",
            str(root),
            "--no-config",
            "--git-diff",
            "HEAD",
            "--view",
            "changes",
            "--format",
            "json",
            "--no-cache",
            "--progress",
            "never",
            "--check",
        ]
        run(
            [sys.executable, "-m", "archbird", *common, "--output", str(python_output)],
            cwd=repository,
            env=python_env,
        )
        node_env = os.environ.copy()
        node_env["ARCHBIRD_ENGINE"] = "native"
        node_env["ARCHBIRD_NATIVE_ADDON"] = str(addon)
        run(
            [node, str(repository / "js/src/cli.js"), *common, "--output", str(node_output)],
            cwd=repository,
            env=node_env,
        )
        expected = {
            "entries": [
                {"path": "README.md", "status": "deleted"},
                {"path": "src/added.js", "status": "added"},
                {
                    "path": "src/new.js",
                    "previous_path": "src/old.js",
                    "status": "renamed",
                },
                {"path": "src/index.js", "status": "modified"},
            ],
            "source": {"identity": "HEAD", "kind": "git-diff"},
        }
        # Canonical order is UTF-8 byte order, so index precedes new.
        expected["entries"].sort(
            key=lambda row: (row["path"], row["status"], row.get("previous_path", ""))
        )
        for label, output in (("Python", python_output), ("Node", node_output)):
            document = json.loads(output.read_text(encoding="utf-8"))
            if document["query"].get("change_set") != expected:
                raise AssertionError(f"{label} Git adapter changed canonical evidence")
            seeds = {row["path"] for row in document["files"] if row["distance"] == 0}
            if not {"src/added.js", "src/index.js", "src/new.js"} <= seeds:
                raise AssertionError(f"{label} Git adapter omitted current seeds: {seeds}")
            if "README.md" in seeds:
                raise AssertionError(f"{label} invented deleted current evidence")

        invalid_change_sets = (
            {"entries": [], "source": {"identity": "x", "kind": "git-diff"}},
            {
                "entries": [{"path": "../bad", "status": "modified"}],
                "source": {"identity": "x", "kind": "git-diff"},
            },
            {
                "entries": [{"path": "new.js", "status": "renamed"}],
                "source": {"identity": "x", "kind": "git-diff"},
            },
            {
                "entries": [
                    {"path": "z.js", "status": "modified"},
                    {"path": "a.js", "status": "modified"},
                ],
                "source": {"identity": "x", "kind": "path-list"},
            },
            {
                "entries": [
                    {"path": "same.js", "status": "modified"},
                    {"path": "same.js", "status": "modified"},
                ],
                "source": {"identity": "x", "kind": "path-list"},
            },
        )
        for change_set in invalid_change_sets:
            try:
                query_map_json(map_output.read_bytes(), change_set=change_set)
            except Exception:
                pass
            else:
                raise AssertionError(f"native core accepted invalid change set: {change_set}")

        markdown = run(
            [
                sys.executable,
                "-m",
                "archbird",
                *common[:-8],
                "--view",
                "changes",
                "--detail",
                "full",
                "--format",
                "markdown",
                "--no-cache",
                "--progress",
                "never",
            ],
            cwd=repository,
            env=python_env,
        ).stdout.decode("utf-8")
        for text in (
            "## Changed paths",
            "modified src/index.js",
            "renamed src/new.js <- src/old.js",
            "deleted README.md [outside map]",
        ):
            if text not in markdown:
                raise AssertionError(f"Git change brief omitted {text!r}")
    print("Python/Node Git-diff change-set parity passed")


if __name__ == "__main__":
    main()
