#!/usr/bin/env python3
"""Keep public quick-start JSON and commands executable."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def marked_json(path: Path, name: str) -> object:
    text = path.read_text(encoding="utf-8")
    start = f"<!-- {name}:start -->\n```json\n"
    end = f"\n```\n<!-- {name}:end -->"
    if text.count(start) != 1 or text.count(end) != 1:
        raise AssertionError(f"{path}: expected one {name} JSON block")
    return json.loads(text.split(start, 1)[1].split(end, 1)[0])


def github_anchor(heading: str) -> str:
    """Return the subset of GitHub heading slugs used by the public READMEs."""
    value = heading.strip().lower()
    value = re.sub(r"[^\w\- ]", "", value, flags=re.UNICODE)
    return value.replace(" ", "-")


def check_public_readmes(repository: Path) -> None:
    readmes = tuple(
        repository / relative
        for relative in ("README.md", "py/README.md", "js/README.md")
    )
    forbidden = (
        "/home/anton",
        "/media/anton",
        "archbird_py",
        "MEMORY.md",
        "LOG.md",
        "PLAN_",
        "FEEDBACK.md",
        "StatSim",
        "codemap",
    )
    tagline = (
        "**Map codebases. Verify architecture. Plan and check structural changes.**"
    )
    unexpected_readmes = tuple(
        path
        for root in (repository / "app", repository / "docs")
        if root.exists()
        for path in root.rglob("README.md")
        if "node_modules" not in path.parts
    )
    if unexpected_readmes:
        raise AssertionError(
            "public documentation must remain one README per core/JS/Python target: "
            + ", ".join(str(path) for path in unexpected_readmes)
        )
    link_pattern = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
    for readme in readmes:
        text = readme.read_text(encoding="utf-8")
        if text.count(tagline) != 1:
            raise AssertionError(f"{readme}: missing the shared product definition")
        if text.count("```") % 2:
            raise AssertionError(f"{readme}: unbalanced fenced code blocks")
        leaked = [marker for marker in forbidden if marker in text]
        if leaked:
            raise AssertionError(f"{readme}: private development marker(s): {leaked}")
        headings = {
            github_anchor(line.lstrip("#").strip())
            for line in text.splitlines()
            if re.match(r"^#{1,6} ", line)
        }
        for raw_target in link_pattern.findall(text):
            target = raw_target.strip().strip("<>")
            if target.startswith(("https://", "http://", "mailto:")):
                continue
            if target.startswith("#"):
                if target[1:] not in headings:
                    raise AssertionError(f"{readme}: unknown heading link {target}")
                continue
            relative = target.split("#", 1)[0]
            if not (readme.parent / relative).resolve().exists():
                raise AssertionError(f"{readme}: missing relative link target {target}")


def run(*arguments: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    source_python = str(Path(__file__).resolve().parents[1] / "py")
    environment["PYTHONPATH"] = (
        source_python
        if not environment.get("PYTHONPATH")
        else source_python + os.pathsep + environment["PYTHONPATH"]
    )
    result = subprocess.run(
        [sys.executable, "-m", "archbird", *arguments],
        cwd=cwd,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(arguments)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    check_public_readmes(repository)
    project_template = repository / "examples" / "minimal.archbird.json"
    verify_template = repository / "examples" / "minimal.verify.json"
    project_config = json.loads(project_template.read_text(encoding="utf-8"))
    verification = json.loads(verify_template.read_text(encoding="utf-8"))
    if project_config.get("schema_version") != 1:
        raise AssertionError("quick-start project config is not schema 1")
    if verification.get("schema_version") != 1:
        raise AssertionError("quick-start verification suite is not schema 1")
    for readme in (
        repository / "README.md",
        repository / "py" / "README.md",
        repository / "js" / "README.md",
    ):
        if marked_json(readme, "archbird-minimal-project-config") != project_config:
            raise AssertionError(f"{readme}: project config drifted from template")
        if marked_json(readme, "archbird-minimal-verify-config") != verification:
            raise AssertionError(f"{readme}: Verify suite drifted from template")

    temporary_root = repository / "build" / "tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as raw:
        root = Path(raw)
        (root / ".archbird").mkdir()
        shutil.copyfile(project_template, root / "archbird.json")
        shutil.copyfile(verify_template, root / "archbird.verify.json")
        write(
            root / "include" / "demo.h",
            "#ifndef DEMO_H\n#define DEMO_H\n"
            "int demo_open(void);\nvoid demo_close(void);\n#endif\n",
        )
        write(
            root / "src" / "core.c",
            '#include "demo.h"\nint demo_open(void) { return 0; }\n'
            "void demo_close(void) {}\n",
        )
        write(
            root / "python" / "demo" / "__init__.py",
            "class Client:\n"
            "    def open(self):\n"
            "        return 0\n",
        )
        write(
            root / "python" / "tests" / "test_client.py",
            "from demo import Client\n\n"
            "def test_client_open():\n"
            "    assert Client().open() == 0\n",
        )
        write(
            root / "python" / "pyproject.toml",
            '[project]\nname = "demo"\nversion = "1.0.0"\n',
        )
        write(
            root / "js" / "src" / "index.ts",
            "export function createClient(): object { return {}; }\n",
        )
        write(
            root / "js" / "test" / "index.test.ts",
            "import { createClient } from '../src/index';\n"
            "test('create client', () => { expect(createClient()).toBeTruthy(); });\n",
        )
        write(
            root / "js" / "package.json",
            '{"name":"demo-js","version":"1.0.0","exports":"./src/index.ts",'
            '"scripts":{"test":"node --test"}}\n',
        )
        write(root / "Makefile", "all:\n\t@echo demo\n")

        run(
            "map",
            ".",
            "--format",
            "json",
            "--output",
            ".archbird/map.json",
            "--check",
            cwd=root,
        )
        run(
            "query",
            "--map",
            ".archbird/map.json",
            "--symbol",
            "demo_open",
            "--depth",
            "1",
            "--max-chars",
            "12000",
            cwd=root,
        )
        run(
            "verify",
            ".",
            "--format",
            "json",
            "--output",
            "verification.json",
            "--check",
            cwd=root,
        )
        result = json.loads((root / "verification.json").read_text(encoding="utf-8"))
        if result["summary"]["checks"] != {
            "fail": 0,
            "not_applicable": 0,
            "pass": 1,
            "unknown": 0,
            "waived": 0,
        }:
            raise AssertionError(
                "unexpected quick-start verification:\n"
                + json.dumps(result, indent=2, sort_keys=True)
            )

        from archbird import Project, audit_map_freshness

        project = Project.from_repository(root, config=root / "archbird.json")
        map_json = project.map_json(pretty=True)
        if project.map()["project"] != "demo":
            raise AssertionError("Python README Project API mapped the wrong project")
        if not project.map_markdown(max_chars=12_000):
            raise AssertionError("Python README Project API rendered empty Markdown")
        if not project.query_markdown(symbols=["demo_open"], depth=1):
            raise AssertionError("Python README Project API rendered empty Query")
        graph = json.loads(project.graph_view_json(view="components"))
        if graph["artifact"] != "archbird-graph-view":
            raise AssertionError("Python README Project API rendered the wrong graph")
        freshness = json.loads(audit_map_freshness(map_json, project.map_json()))
        if freshness["status"] != "current":
            raise AssertionError("Python README freshness example is not current")


if __name__ == "__main__":
    main()
