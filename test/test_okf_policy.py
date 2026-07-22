#!/usr/bin/env python3
"""Host-independent regression corpus for the native OKF policy kernel."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def canonical(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, ensure_ascii=False, separators=(",", ":")
    ).encode()


def row(path: str, frontmatter: dict, body: str, links=()) -> dict:
    raw = canonical([frontmatter, body, links])
    type_name = frontmatter.get("type", "")
    title = frontmatter.get("title", "")
    description = frontmatter.get("description", "")
    tags = frontmatter.get("tags", [])
    return {
        "body": body,
        "byte_length": len(raw),
        "casefold": {
            "tags": sorted({value.casefold() for value in tags})
            if isinstance(tags, list)
            and all(isinstance(value, str) for value in tags)
            else [],
            "text": "\n".join(
                (title, description, type_name, body)
            ).casefold(),
            "type": type_name.casefold(),
        },
        "frontmatter": frontmatter,
        "links": list(links),
        "path": path,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "state": "current",
    }


def link(label: str, href: str, *, path: str, external=False, fragment=False):
    return {
        "external": external,
        "fragment_only": fragment,
        "href": href,
        "label": label,
        "path": path,
        "repr": repr(href),
    }


def source_bundle() -> dict:
    documents = [
        row(
            "component.md",
            {
                "type": "Component",
                "title": "Straße renderer",
                "tags": ["Core", "checked"],
                "archbird": {
                    "relations": [
                        {"kind": "governed_by", "target": "requirement"}
                    ]
                },
            },
            "See [requirement](requirement.md), [missing](missing.md), "
            "[root](/index.md), [anchor](#details), and "
            "[external](https://example.test/x).\n",
            (
                link("anchor", "#details", path="", fragment=True),
                link(
                    "external",
                    "https://example.test/x",
                    path="/x",
                    external=True,
                ),
                link("missing", "missing.md", path="missing.md"),
                link("requirement", "requirement.md", path="requirement.md"),
                link("root", "/index.md", path="/index.md"),
            ),
        ),
        row("index.md", {"okf_version": "0.1"}, "# Bundle\n"),
        row(
            "requirement.md",
            {
                "type": "Requirement",
                "title": "Explicit requirement",
                "archbird": {
                    "entity": {"kind": "requirement", "id": "REQ-1"}
                },
            },
            "# Requirement\n\nProse mentions REQ-NOT-EXPLICIT.\n",
        ),
    ]
    return {
        "artifact": "okf-source-bundle",
        "diagnostics": [],
        "documents": documents,
        "known_paths": ["component.md", "index.md", "requirement.md"],
        "producer": {
            "implementation_sha256": "0" * 64,
            "name": "fixture-okf-parser",
            "runtime": "fixture-1",
            "version": "1",
        },
        "schema_version": 1,
    }


def query(**values) -> dict:
    result = {
        "artifact": "okf-query-input",
        "concepts": [],
        "requirements": [],
        "schema_version": 1,
        "tags": [],
        "text": [],
        "types": [],
    }
    for key, rows in values.items():
        if key in {"types", "tags", "text"}:
            result[key] = [
                {"value": value, "casefold": value.casefold()} for value in rows
            ]
        else:
            result[key] = list(rows)
    return result


def run(binary: Path, source: Path, query_path: str = "-", format="json"):
    return subprocess.run(
        [str(binary), format, str(source), query_path, "1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    temp_root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path("build")
    temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temp_root) as directory:
        root = Path(directory)
        source_path = root / "source.json"
        source_path.write_bytes(canonical(source_bundle()))

        first = run(binary, source_path)
        second = run(binary, source_path)
        assert first.returncode == second.returncode == 0, first.stderr
        assert first.stdout == second.stdout
        index = json.loads(first.stdout)
        assert index["artifact"] == "okf-index"
        assert index["summary"] == {
            "broken_links": 1,
            "concepts": 2,
            "documents": 3,
            "errors": 0,
            "requirements": 1,
            "warnings": 1,
        }
        assert {
            (row["requirement_id"], row["concept_id"], row["source"])
            for row in index["requirement_links"]
        } == {
            ("REQ-1", "component", "typed_relation"),
            ("REQ-1", "requirement", "explicit"),
        }
        assert not any(
            row["requirement_id"] == "REQ-NOT-EXPLICIT"
            for row in index["requirement_links"]
        )

        query_path = root / "query.json"
        query_path.write_bytes(
            canonical(
                query(
                    requirements=["REQ-1"],
                    tags=["core"],
                    text=["STRASSE"],
                    types=["component"],
                )
            )
        )
        selected = run(binary, source_path, str(query_path))
        assert selected.returncode == 0, selected.stderr
        selected_data = json.loads(selected.stdout)
        assert [row["concept_id"] for row in selected_data["documents"]] == [
            "component"
        ]
        assert selected_data["documents"][0]["title"] == "Straße renderer"

        markdown = run(binary, source_path, str(query_path), "markdown")
        assert markdown.returncode == 0, markdown.stderr
        assert markdown.stdout.startswith(b"# OKF query\n")
        assert b"Prose is never translated into constraints." in markdown.stdout

        unmatched_path = root / "unmatched.json"
        unmatched_path.write_bytes(canonical(query(concepts=["absent/**"])))
        unmatched = run(binary, source_path, str(unmatched_path))
        assert unmatched.returncode != 0
        assert b"selectors matched no concepts" in unmatched.stderr

        malformed = source_bundle()
        malformed["documents"] = list(reversed(malformed["documents"]))
        malformed_path = root / "malformed.json"
        malformed_path.write_bytes(canonical(malformed))
        rejected = run(binary, malformed_path)
        assert rejected.returncode != 0
        assert b"sorted and unique" in rejected.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
