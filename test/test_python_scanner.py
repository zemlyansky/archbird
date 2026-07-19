#!/usr/bin/env python3
"""Conformance probes for the portable native Python lexical provider."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import subprocess
import sys


CASES = (
    (
        "structure",
        "py/pkg/api.py",
        "# def fake(): pass\n"
        "import os, pkg.mod as mod\n"
        "from .dep import Thing as Alias, helper\n"
        "VALUE: int = 1\n\n"
        "configure(debug=True)\n\n"
        "class Box(Base):\n"
        "    def run(self, x):\n"
        "        return helper(x) + self.convert(x)\n\n"
        "async def top(x):\n"
        "    return Box().run(x)\n",
        {
            "symbols": {
                ("Box", "class", "class"),
                ("Box.run", "method", "method"),
                ("VALUE", "variable", "module"),
                ("top", "function", "function"),
            },
            "calls": {"Box": 1, "configure": 1, "helper": 1},
            "methods": {"convert": 1, "run": 1},
            "imports": {".dep", "os", "pkg.mod"},
            "exports": {"Box", "VALUE", "top"},
            "reexports": {"Alias", "helper", "mod", "os"},
        },
    ),
    (
        "package",
        "pkg/__init__.py",
        "\"\"\"class Fake:\n    pass\n\"\"\"\n"
        "from .api import Public, helper as renamed\n"
        "import pkg.extra as extra\n"
        "def café():\n"
        "    return Public()\n",
        {
            "symbols": {("café", "function", "function")},
            "calls": {"Public": 1},
            "methods": {},
            "imports": {".api", "pkg.extra"},
            "exports": {"Public", "café", "extra", "renamed"},
            "reexports": {"Public", "extra", "renamed"},
        },
    ),
    (
        "malformed",
        "broken.py",
        "def partial(value\n    return real(value)\n",
        {
            "symbols": {("partial", "function", "function")},
            "calls": {"real": 1},
            "methods": {},
            "imports": set(),
            "exports": {"partial"},
            "reexports": set(),
        },
    ),
    (
        "logical_scope_boundaries",
        "py/pkg/scopes.py",
        "class Container:\n"
        "    def method(self):\n"
        "        ratio = 4 // (2 * 1)\n"
        "        def first():\n"
        "            return 1\n\n"
        "        if self.flag:\n"
        "            def branch():\n"
        "                return 2\n"
        "        elif self.other:\n"
        "            def branch():\n"
        "                return 3\n\n"
        "        value = one + \\\n"
        "            two\n"
        "        def second(\n"
        "            value: tuple[\n"
        "                int,\n"
        "            ],\n"
        "        ):\n"
        "            return value\n\n"
        "    def sibling(self):\n"
        "        return None\n\n"
        "def top():\n"
        "    return None\n",
        {
            "symbols": {
                ("Container", "class", "class"),
                ("Container.method", "method", "method"),
                ("Container.method.branch", "method", "method"),
                ("Container.method.first", "method", "method"),
                ("Container.method.second", "method", "method"),
                ("Container.sibling", "method", "method"),
                ("top", "function", "function"),
            },
            "calls": {},
            "methods": {},
            "imports": set(),
            "exports": {"Container", "top"},
            "reexports": set(),
        },
    ),
)


def projection(document: dict) -> dict:
    symbols = set()
    calls: Counter[str] = Counter()
    methods: Counter[str] = Counter()
    imports = set()
    exports = set()
    reexports = set()
    for fact in document["facts"]:
        domain = fact["domain"]
        if domain == "symbols":
            symbols.add(
                (
                    fact["name"],
                    fact["kind"],
                    fact["attributes"]["scope"],
                )
            )
        elif domain == "calls":
            calls[fact["name"]] += 1
        elif domain == "method-calls":
            methods[fact["name"]] += 1
        elif domain == "imports":
            imports.add(fact["name"])
        elif domain == "exports":
            exports.add(fact["name"])
        elif domain == "reexport-candidates":
            reexports.add(fact["name"])
    return {
        "symbols": symbols,
        "calls": dict(sorted(calls.items())),
        "methods": dict(sorted(methods.items())),
        "imports": imports,
        "exports": exports,
        "reexports": reexports,
    }


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_python_scanner.py SCANNER_CLI")
    executable = Path(sys.argv[1]).resolve()
    for name, path, source, expected in CASES:
        command = [str(executable), "python", path]
        first = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        second = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        if first.stdout != second.stdout:
            raise AssertionError(f"{name}: provider bytes are not deterministic")
        document = json.loads(first.stdout)
        if document["producer"]["name"] != "archbird-native-python-lexical":
            raise AssertionError(f"{name}: wrong provider identity")
        if {row["coverage"] for row in document["capabilities"]} != {"bounded"}:
            raise AssertionError(f"{name}: lexical coverage was overstated")
        actual = projection(document)
        if actual != expected:
            raise AssertionError(
                f"{name}: native Python lexical projection differs\n"
                f"actual={actual!r}\nexpected={expected!r}"
            )
    invalid = subprocess.run(
        [str(executable), "python", "invalid.py"],
        input=b"name = \xff\n",
        capture_output=True,
        check=True,
    )
    invalid_document = json.loads(invalid.stdout)
    if invalid_document["facts"] or {
        row["coverage"] for row in invalid_document["capabilities"]
    } != {"none"}:
        raise AssertionError("invalid source token claimed lexical coverage")
    if [row["code"] for row in invalid_document["diagnostics"]] != [
        "python-lexical-encoding-inapplicable"
    ]:
        raise AssertionError(invalid_document["diagnostics"])
    print(f"native Python lexical conformance passed: {len(CASES)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
