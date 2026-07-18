#!/usr/bin/env python3
"""Differential native OKF policy against a maintainer reference."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def purge_archbird() -> None:
    for name in tuple(sys.modules):
        if name == "archbird" or name.startswith("archbird."):
            del sys.modules[name]


def native(binary: Path, source: Path, query: Path | None, format: str) -> bytes:
    result = subprocess.run(
        [
            str(binary),
            format,
            str(source),
            str(query) if query else "-",
            "1" if query else "0",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise AssertionError(result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def normalized_json(expected: str, actual: bytes) -> None:
    reference = json.loads(expected)
    subject = json.loads(actual)
    reference["tool"] = subject["tool"]
    if reference != subject:
        raise AssertionError("native OKF JSON differs from the reference")


def main_fixture(root: Path) -> None:
    (root / "index.md").write_text(
        "---\nokf_version: '0.1'\n---\n# Bundle\n", encoding="utf-8"
    )
    (root / "requirement.md").write_text(
        """---
type: Requirement
title: Explicit requirement
timestamp: 2026-07-11T10:00:00Z
unknown_field: {nested: true}
archbird:
  entity: {kind: requirement, id: REQ-EXPLICIT}
---
# Requirement

The prose also mentions REQ-PROSE, which is not metadata.
""",
        encoding="utf-8",
    )
    (root / "component.md").write_text(
        """---
type: Component
tags: [core, checked]
archbird:
  relations:
    - {kind: governed_by, target: requirement}
---
See [the requirement](requirement.md) and [future knowledge](missing.md).
""",
        encoding="utf-8",
    )
    (root / "unicode.md").write_text(
        """---
type: Straße
title: ﬀ renderer
tags: [Maße]
---
# Unicode

Σς behavior.
""",
        encoding="utf-8",
    )


def invalid_fixture(root: Path) -> None:
    (root / "index.md").write_text("no section heading\n", encoding="utf-8")
    (root / "duplicate.md").write_text(
        "---\ntype: One\ntype: Two\n---\nbody\n", encoding="utf-8"
    )
    nested = root / "nested"
    nested.mkdir()
    (nested / "index.md").write_text(
        "---\ntype: Invalid\n---\n# Index\n", encoding="utf-8"
    )
    (root / "log.md").write_text(
        "# Log\n\n## yesterday\n* update\n", encoding="utf-8"
    )


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    target = Path(sys.argv[2]).resolve()
    oracle = Path(sys.argv[3]).resolve()
    temp_root = Path(sys.argv[4]).resolve()
    temp_root.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, str(target / "py"))
    from archbird.adapters.okf.parser import okf_query_input, parse_okf_bundle

    with tempfile.TemporaryDirectory(dir=temp_root) as directory:
        work = Path(directory)
        main_root = work / "main"
        invalid_root = work / "invalid"
        main_root.mkdir()
        invalid_root.mkdir()
        main_fixture(main_root)
        invalid_fixture(invalid_root)
        main_source = work / "main-source.json"
        invalid_source = work / "invalid-source.json"
        main_source.write_bytes(parse_okf_bundle(main_root))
        invalid_source.write_bytes(parse_okf_bundle(invalid_root))
        queries = {
            "requirement": okf_query_input(requirements=["REQ-EXPLICIT"]),
            "tag": okf_query_input(tags=["core"]),
            "unicode": okf_query_input(
                types=["STRASSE"], tags=["MASSE"], text=["FF", "ΣΣ"]
            ),
        }
        query_paths = {}
        for name, encoded in queries.items():
            query_paths[name] = work / f"{name}.json"
            query_paths[name].write_bytes(encoded)

        sys.path.remove(str(target / "py"))
        purge_archbird()
        sys.path.insert(0, str(oracle))
        from archbird.interchange.okf.reader import (
            index_okf_bundle,
            query_okf,
            render_okf_json,
            render_okf_markdown,
        )

        for root, source in ((main_root, main_source), (invalid_root, invalid_source)):
            data = index_okf_bundle(root)
            first = native(binary, source, None, "json")
            second = native(binary, source, None, "json")
            if first != second:
                raise AssertionError("native OKF index is nondeterministic")
            normalized_json(render_okf_json(data), first)
            if native(binary, source, None, "markdown") != render_okf_markdown(
                data
            ).encode():
                raise AssertionError("native OKF Markdown index differs")

        data = index_okf_bundle(main_root)
        selections = {
            "requirement": query_okf(data, requirements=["REQ-EXPLICIT"]),
            "tag": query_okf(data, tags=["core"]),
            "unicode": query_okf(
                data, types=["STRASSE"], tags=["MASSE"], text=["FF", "ΣΣ"]
            ),
        }
        for name, selected in selections.items():
            actual = native(binary, main_source, query_paths[name], "json")
            normalized_json(
                render_okf_json(data, documents=selected, include_body=True),
                actual,
            )
            markdown = native(binary, main_source, query_paths[name], "markdown")
            if markdown != render_okf_markdown(data, documents=selected).encode():
                raise AssertionError(f"native OKF Markdown query differs: {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
