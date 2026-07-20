#!/usr/bin/env python3
"""Require Python and Node hosts to emit identical discovery resolution."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_resolution_frontend_parity.py "
            "PY_EXTENSION NODE ADDON REPOSITORY"
        )
    extension, node, addon, repository = map(Path, sys.argv[1:])
    repository = repository.resolve()
    fixture = repository / "test/fixtures/zero_config"
    sys.path.insert(0, str(repository / "py"))
    load_extension(extension.resolve())
    from archbird.native import Project, resolve_discovery

    python_outputs = [
        resolve_discovery(fixture),
        resolve_discovery(
            fixture,
            project="cli",
            ignore_files=(".customignore",),
            max_file_bytes=100,
            max_index_bytes=1000,
        ),
        resolve_discovery(
            fixture,
            ignore=False,
            ignore_files=(".customignore",),
        ),
    ]
    completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(fixture),
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    node_outputs = [
        bytes.fromhex(row) for row in completed.stdout.decode().splitlines()
    ]
    if node_outputs != python_outputs:
        raise AssertionError("Python and Node config-resolution artifacts differ")
    r_fixture = repository / "test/fixtures/zero_config_r"
    r_resolution = resolve_discovery(r_fixture)
    r_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(r_fixture),
            "default",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if bytes.fromhex(r_completed.stdout.decode().strip()) != r_resolution:
        raise AssertionError("Python and Node CRAN resolution artifacts differ")
    compiler_fixture = repository / "build/tmp/config-resolution-compiler"
    shutil.rmtree(compiler_fixture, ignore_errors=True)
    (compiler_fixture / "src").mkdir(parents=True)
    (compiler_fixture / "src/main.c").write_text("int main(void) { return 0; }\n")
    (compiler_fixture / "compile_commands.json").write_text("[]\n")
    (compiler_fixture / "index.scip").write_bytes(b"fixture")
    compiler_resolution = resolve_discovery(compiler_fixture)
    compiler_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(compiler_fixture),
            "default",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if bytes.fromhex(compiler_completed.stdout.decode().strip()) != compiler_resolution:
        raise AssertionError("Python and Node compiler resolution artifacts differ")
    compiler_document = json.loads(compiler_resolution)
    if compiler_document["effective_config"]["builds"] != [
        {
            "kind": "compile_commands",
            "name": "compile_commands",
            "path": "compile_commands.json",
            "variant": "default",
        }
    ] or compiler_document["effective_config"]["indexes"] != [
        {"format": "scip", "name": "scip", "path": "index.scip", "required": True}
    ]:
        raise AssertionError(
            "zero-config compiler evidence is incorrect: "
            f"{compiler_document['effective_config']!r}"
        )
    shutil.rmtree(compiler_fixture)
    r_project = Project.from_repository(r_fixture, jobs=1)
    r_map = json.loads(r_project.map_json())
    if r_map["project"] != "zeroR":
        raise AssertionError(f"DESCRIPTION identity was lost: {r_map['project']!r}")
    if len(r_map["packages"]) != 1:
        raise AssertionError(f"CRAN package was not inferred: {r_map['packages']!r}")
    r_package = r_map["packages"][0]
    if (
        r_package["identity"] != "zeroR"
        or r_package["version"] != "1.2.3"
        or r_package["manifest"] != "DESCRIPTION"
        or r_package["exports"] != ["alpha", "beta"]
    ):
        raise AssertionError(f"CRAN package evidence is incorrect: {r_package!r}")
    autoconf_fixture = repository / "test/fixtures/zero_config_autoconf"
    autoconf_resolution = resolve_discovery(autoconf_fixture)
    autoconf_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(autoconf_fixture),
            "default",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if bytes.fromhex(autoconf_completed.stdout.decode().strip()) != autoconf_resolution:
        raise AssertionError("Python and Node Autoconf resolution artifacts differ")
    autoconf_project = Project.from_repository(autoconf_fixture, jobs=1)
    autoconf_map = json.loads(autoconf_project.map_json())
    if autoconf_map["project"] != "native-demo":
        raise AssertionError(f"AC_INIT identity was lost: {autoconf_map['project']!r}")
    if len(autoconf_map["packages"]) != 1:
        raise AssertionError(
            f"Autoconf package was not inferred: {autoconf_map['packages']!r}"
        )
    autoconf_package = autoconf_map["packages"][0]
    if (
        autoconf_package["identity"] != "native-demo"
        or autoconf_package["version"] != "2.4"
        or autoconf_package["manifest"] != "configure.ac"
        or autoconf_package["kind"] != "generic"
    ):
        raise AssertionError(
            f"Autoconf package evidence is incorrect: {autoconf_package!r}"
        )
    routes = {route["name"]: route for route in autoconf_map["builds"]}
    if routes["autoreconf"]["command"] != "autoreconf -i" or routes["configure"][
        "paths"
    ] != ["Makefile", "config.h", "config.status", "src/Makefile"]:
        raise AssertionError(f"Autoconf build routes are incorrect: {routes!r}")
    print(
        "Python/Node config-resolution parity passed for npm, Python, CRAN, "
        "Autoconf, SCIP, and compile_commands"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
