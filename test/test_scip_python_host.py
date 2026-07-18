#!/usr/bin/env python3
"""Exercise the host-neutral SCIP provider through the CPython frontend."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


def varint(value: int) -> bytes:
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        result.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(result)


def field_key(field: int, wire: int) -> bytes:
    return varint(field * 8 + wire)


def integer_field(field: int, value: int) -> bytes:
    return field_key(field, 0) + varint(value)


def bytes_field(field: int, value: bytes) -> bytes:
    return field_key(field, 2) + varint(len(value)) + value


def string_field(field: int, value: str) -> bytes:
    return bytes_field(field, value.encode())


def occurrence(symbol: str, roles: int, line: int, start: int, end: int) -> bytes:
    source_range = b"".join(
        (
            integer_field(1, line),
            integer_field(2, start),
            integer_field(3, end),
        )
    )
    return b"".join(
        (
            string_field(2, symbol),
            integer_field(3, roles),
            bytes_field(8, source_range),
        )
    )


def document(path: str, occurrence_value: bytes, text: str) -> bytes:
    return b"".join(
        (
            string_field(1, path),
            bytes_field(2, occurrence_value),
            string_field(4, "javascript"),
            string_field(5, text),
        )
    )


def scip_fixture() -> bytes:
    symbol = "scip host fixture 1.0 fixture/add()."
    tool = string_field(1, "archbird-python-fixture") + string_field(2, "1.0")
    metadata = bytes_field(2, tool) + integer_field(4, 1)
    definition = "export function add(a, b) { return a + b; }\n"
    reference = 'import { add } from "./defs.js";\nexport const value = add(1, 2);\n'
    return b"".join(
        (
            bytes_field(1, metadata),
            bytes_field(
                2,
                document(
                    "src/defs.js",
                    occurrence(symbol, 1, 0, 16, 19),
                    definition,
                ),
            ),
            bytes_field(
                2,
                document(
                    "src/use.js",
                    occurrence(symbol, 8, 0, 9, 12),
                    reference,
                ),
            ),
        )
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_scip_python_host.py EXTENSION REPOSITORY")
    extension = Path(sys.argv[1]).resolve()
    repository = Path(sys.argv[2]).resolve()
    sys.path.insert(0, str(repository / "py"))
    load_extension(extension)
    from archbird.native import Project, Source

    definitions = b"export function add(a, b) { return a + b; }\n"
    uses = b'import { add } from "./defs.js";\nexport const value = add(1, 2);\n'
    project = Project(
        "scip-python",
        (
            Source("index.scip", scip_fixture(), roles=("index",)),
            Source(
                "src/defs.js",
                definitions,
                language="javascript",
                layer="javascript",
            ),
            Source(
                "src/use.js",
                uses,
                language="javascript",
                layer="javascript",
            ),
        ),
    )
    project.set_config(
        json.dumps(
            {
                "schema_version": 1,
                "project": "scip-python",
                "layers": [
                    {
                        "name": "javascript",
                        "language": "javascript",
                        "globs": ["src/**/*.js"],
                    }
                ],
                "indexes": [
                    {
                        "name": "compiler",
                        "format": "scip",
                        "path": "index.scip",
                        "position_encoding_fallback": "utf8",
                    }
                ],
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
    )
    project.scan()
    mapped = project.map()
    semantic_edges = [
        edge for edge in mapped["edges"] if edge["kind"] == "semantic-reference"
    ]
    if semantic_edges != [
        {
            "evidence": [
                {
                    "basis": "semantic-index",
                    "provider": "compiler",
                    "state": "current",
                }
            ],
            "kind": "semantic-reference",
            "names": ["add"],
            "source": "src/use.js",
            "target": "src/defs.js",
        }
    ]:
        raise AssertionError(f"unexpected Python-host SCIP edge: {semantic_edges!r}")
    providers = [project.provider_facts(index) for index in range(project.counts["providers"])]
    semantic = next(
        row for row in providers if row["producer"]["name"] == "archbird-scip"
    )
    definition = next(
        fact for fact in semantic["facts"] if fact["domain"] == "semantic-definitions"
    )
    if (
        definition.get("name") != "add"
        or definition["attributes"]["display_name_state"] != "source-range"
        or definition["attributes"]["semantic_symbol"]
        != "scip host fixture 1.0 fixture/add()."
    ):
        raise AssertionError(f"SCIP identity/display projection is incomplete: {definition!r}")
    print("CPython host-neutral SCIP Map passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
