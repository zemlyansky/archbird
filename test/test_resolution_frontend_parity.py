#!/usr/bin/env python3
"""Require Python and Node hosts to emit identical discovery resolution."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys


MAP_REQUEST = json.dumps(
    {
        "artifact": "archbird-map-request",
        "default_excludes": True,
        "exclude": [],
        "ignore": True,
        "only": [],
        "schema_version": 1,
        "sources": [],
    },
    sort_keys=True,
    separators=(",", ":"),
).encode()


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def raw_inventory(
    files: list[dict[str, object]], documents: list[tuple[str, bytes]]
) -> bytes:
    return canonical(
        {
            "artifact": "archbird-repository-inventory",
            "documents": [
                {"content_hex": content.hex(), "path": path}
                for path, content in documents
            ],
            "files": files,
            "ignore_files": [],
            "schema_version": 1,
        }
    )


def expect_native_rejection(native_module: object, inventory: bytes, text: str) -> None:
    try:
        native_module.discovery_resolve(b"", MAP_REQUEST, inventory)
    except RuntimeError as error:
        if text not in str(error):
            raise AssertionError(f"wrong native rejection: {error}") from error
    else:
        raise AssertionError(f"native discovery accepted invalid inventory: {text}")


def check_native_manifest_protocol(native_module: object) -> None:
    root = b'{"workspaces":["packages/*"]}'
    nested = b'{"name":"@fixture/a","version":"1.0.0"}'
    files = [
        {"bytes": len(root), "path": "package.json"},
        {"bytes": len(nested), "path": "packages/a/package.json"},
    ]
    candidate = json.loads(
        native_module.discovery_resolve(
            b"",
            MAP_REQUEST,
            raw_inventory(files, [("package.json", root)]),
        )
    )
    if candidate["manifest_requests"] != [
        {
            "evidence": ["package.json", "packages/*"],
            "fulfilled": False,
            "kind": "npm",
            "max_bytes": 262144,
            "path": "packages/a/package.json",
            "source": "npm-workspace",
        }
    ]:
        raise AssertionError(
            f"native manifest handshake is wrong: {candidate['manifest_requests']!r}"
        )
    if candidate["diagnostics"] != [
        {
            "code": "discovery-manifest-input-missing",
            "path": "packages/a/package.json",
            "severity": "warning",
        }
    ]:
        raise AssertionError(
            f"missing-input evidence is wrong: {candidate['diagnostics']!r}"
        )

    unrequested = raw_inventory(
        files,
        [
            ("package.json", root),
            ("other/package.json", b'{"name":"other"}'),
        ],
    )
    expect_native_rejection(native_module, unrequested, "unrequested manifest")
    mismatched = raw_inventory(
        files,
        [("package.json", root), ("packages/a/package.json", b"{}")],
    )
    expect_native_rejection(native_module, mismatched, "bytes disagree")

    malformed_bytes = b"{"
    malformed_files = [
        {"bytes": len(root), "path": "package.json"},
        {
            "bytes": len(malformed_bytes),
            "path": "packages/a/package.json",
        },
    ]
    malformed = json.loads(
        native_module.discovery_resolve(
            b"",
            MAP_REQUEST,
            raw_inventory(
                malformed_files,
                [
                    ("package.json", root),
                    ("packages/a/package.json", malformed_bytes),
                ],
            ),
        )
    )
    if malformed["diagnostics"] != [
        {
            "code": "discovery-manifest-invalid",
            "path": "packages/a/package.json",
            "severity": "warning",
        }
    ] or not malformed["manifest_requests"][0]["fulfilled"]:
        raise AssertionError(f"malformed candidate policy is wrong: {malformed!r}")

    partial = b'{"workspaces":["packages/*",7]}'
    partial_files = [
        {"bytes": len(partial), "path": "package.json"},
        {"bytes": len(nested), "path": "packages/a/package.json"},
    ]
    partial_resolution = json.loads(
        native_module.discovery_resolve(
            b"",
            MAP_REQUEST,
            raw_inventory(partial_files, [("package.json", partial)]),
        )
    )
    if partial_resolution["manifest_requests"] or partial_resolution[
        "diagnostics"
    ] != [
        {
            "code": "discovery-workspace-shape-unsupported",
            "path": "package.json",
            "severity": "warning",
        }
    ]:
        raise AssertionError(
            "a partially valid npm workspace array was not rejected atomically"
        )

    unsafe = b'{"workspaces":["../packages/*"]}'
    unsafe_resolution = json.loads(
        native_module.discovery_resolve(
            b"",
            MAP_REQUEST,
            raw_inventory(
                [
                    {"bytes": len(unsafe), "path": "package.json"},
                    {"bytes": len(nested), "path": "packages/a/package.json"},
                ],
                [("package.json", unsafe)],
            ),
        )
    )
    if unsafe_resolution["manifest_requests"] or unsafe_resolution[
        "diagnostics"
    ] != [
        {
            "code": "discovery-workspace-pattern-unsupported",
            "limit": 0,
            "path": "package.json",
            "patterns": 1,
            "severity": "warning",
        }
    ]:
        raise AssertionError(
            f"unsafe workspace pattern policy is wrong: {unsafe_resolution!r}"
        )

    python_manifest = b'[project]\nname = "demo"\nversion = "1.0.0"\n'
    ambiguous_python = json.loads(
        native_module.discovery_resolve(
            b"",
            MAP_REQUEST,
            raw_inventory(
                [
                    {"bytes": 10, "path": "py/demo/__init__.py"},
                    {
                        "bytes": len(python_manifest),
                        "path": "py/pyproject.toml",
                    },
                    {"bytes": 10, "path": "py/src/demo/__init__.py"},
                ],
                [("py/pyproject.toml", python_manifest)],
            ),
        )
    )
    if ambiguous_python["diagnostics"] != [
        {
            "code": "discovery-python-import-root-conflict",
            "limit": 1,
            "matches": 2,
            "path": "py/pyproject.toml",
            "severity": "warning",
        }
    ] or ambiguous_python["effective_config"].get("packages", []) or ambiguous_python[
        "project"
    ] != "repository":
        raise AssertionError(
            "ambiguous Python layouts were asserted as package/import truth: "
            f"{ambiguous_python!r}"
        )

    unsupported_layouts = [
        (
            b'[project]\nname = "demo"\n'
            b'[tool.setuptools.packages.find]\n'
            b'where = ["src", "lib"]\n',
            "multiple explicit source roots",
        ),
        (
            b'[project]\nname = "demo"\n'
            b'[tool.setuptools]\npackages = ["demo"]\n'
            b'[tool.setuptools.packages.find]\ninclude = ["other*"]\n',
            "conflicting explicit module hints",
        ),
        (
            b'[project]\nname = "demo"\n'
            b'[tool.setuptools.packages.find]\n'
            b'where = ["src"] trailing\n',
            "trailing unsupported array content",
        ),
        (
            b'[project]\nname = "demo"\n'
            b'[tool.setuptools]\npackages = ["demo/other"]\n',
            "non-importable explicit package names",
        ),
    ]
    for layout_manifest, explanation in unsupported_layouts:
        layout_resolution = json.loads(
            native_module.discovery_resolve(
                b"",
                MAP_REQUEST,
                raw_inventory(
                    [
                        {
                            "bytes": len(layout_manifest),
                            "path": "layout/pyproject.toml",
                        },
                        {
                            "bytes": 10,
                            "path": "layout/src/demo/__init__.py",
                        },
                    ],
                    [("layout/pyproject.toml", layout_manifest)],
                ),
            )
        )
        if layout_resolution["diagnostics"] != [
            {
                "code": "discovery-python-layout-unsupported",
                "path": "layout/pyproject.toml",
                "severity": "warning",
            }
        ] or layout_resolution["effective_config"].get(
            "packages", []
        ) or layout_resolution["project"] != "repository":
            raise AssertionError(
                f"{explanation} was asserted as Python layout truth: "
                f"{layout_resolution!r}"
            )

    supported_layouts = [
        (
            b'[project]\nname = "namespace-demo"\n'
            b'[tool.setuptools.packages.find]\n'
            b'where = ["src"]\ninclude = ["demo*"]\n',
            "namespace/src/demo/module.py",
            "namespace/src",
            "namespace-demo",
        ),
        (
            b'[project]\nname = "stub-dist"\n'
            b'[tool.setuptools]\npackages = ["stub_pkg"]\n',
            "stubs/stub_pkg.pyi",
            "stubs",
            "stub-dist",
        ),
    ]
    for layout_manifest, source_path, import_root, identity in supported_layouts:
        manifest_path = f"{source_path.split('/', 1)[0]}/pyproject.toml"
        layout_resolution = json.loads(
            native_module.discovery_resolve(
                b"",
                MAP_REQUEST,
                raw_inventory(
                    [
                        {"bytes": len(layout_manifest), "path": manifest_path},
                        {"bytes": 10, "path": source_path},
                    ],
                    [(manifest_path, layout_manifest)],
                ),
            )
        )
        python_layer = next(
            layer
            for layer in layout_resolution["effective_config"]["layers"]
            if layer["name"] == "auto-python"
        )
        packages = layout_resolution["effective_config"].get("packages", [])
        if (
            layout_resolution["diagnostics"]
            or python_layer.get("import_roots") != [import_root]
            or [package.get("identity") for package in packages] != [identity]
        ):
            raise AssertionError(
                "a source-backed namespace/stub layout was not discovered: "
                f"{layout_resolution!r}"
            )

    duplicate_identity = b'{"name":"@fixture/shared"}'
    duplicate_packages = json.loads(
        native_module.discovery_resolve(
            b"",
            MAP_REQUEST,
            raw_inventory(
                [
                    {"bytes": len(root), "path": "package.json"},
                    {
                        "bytes": len(duplicate_identity),
                        "path": "packages/a/package.json",
                    },
                    {
                        "bytes": len(duplicate_identity),
                        "path": "packages/b/package.json",
                    },
                ],
                [
                    ("package.json", root),
                    ("packages/a/package.json", duplicate_identity),
                    ("packages/b/package.json", duplicate_identity),
                ],
            ),
        )
    )
    if duplicate_packages["diagnostics"] != [
        {
            "code": "discovery-package-identity-conflict",
            "path": "packages/b/package.json",
            "severity": "warning",
        }
    ] or [
        package.get("identity")
        for package in duplicate_packages["effective_config"]["packages"]
    ] != ["@fixture/shared"]:
        raise AssertionError(
            f"duplicate workspace identity policy is wrong: {duplicate_packages!r}"
        )


def check_bounded_manifest_discovery(
    resolve_discovery: object,
    node: Path,
    addon: Path,
    repository: Path,
) -> None:
    fixture = repository / "build/tmp/config-resolution-manifest-bounds"
    shutil.rmtree(fixture, ignore_errors=True)
    fixture.mkdir(parents=True)
    try:
        (fixture / "package.json").write_text(
            '{"name":"bounds","workspaces":["packages/*"]}\n'
        )
        (fixture / "pyproject.toml").write_text(
            '[tool.uv.workspace]\nmembers = ["python/*"]\n'
        )
        for index in range(130):
            package = fixture / f"packages/p{index:03d}"
            package.mkdir(parents=True)
            (package / "package.json").write_text(
                json.dumps(
                    {
                        "name": f"@bounds/p{index:03d}",
                        "version": "1.0.0",
                    },
                    separators=(",", ":"),
                )
            )
        oversized = fixture / "packages/zz-oversized"
        oversized.mkdir(parents=True)
        (oversized / "package.json").write_bytes(b"{" + b" " * 262144)
        outside = fixture / "outside-package.json"
        outside.write_text('{"name":"outside"}\n')
        linked = fixture / "packages/symlink"
        linked.mkdir(parents=True)
        (linked / "package.json").symlink_to(outside)
        for index in range(34):
            package = fixture / f"python/p{index:03d}"
            module = f"py_pkg_{index:03d}"
            source = package / "src" / module
            source.mkdir(parents=True)
            (source / "__init__.py").write_text("VALUE = 1\n")
            (package / "pyproject.toml").write_text(
                "[project]\n"
                f'name = "py-pkg-{index:03d}"\n'
                'version = "1.0.0"\n\n'
                "[tool.setuptools.packages.find]\n"
                'where = ["src"]\n'
                f'include = ["{module}*"]\n'
            )

        resolution_bytes = resolve_discovery(fixture)
        completed = subprocess.run(
            [
                str(node),
                str(repository / "test/test_resolution_node.js"),
                str(addon.resolve()),
                str(repository),
                str(fixture),
                "default",
            ],
            check=True,
            stdout=subprocess.PIPE,
        )
        if bytes.fromhex(completed.stdout.decode().strip()) != resolution_bytes:
            raise AssertionError("Python and Node bounded-manifest artifacts differ")
        resolution = json.loads(resolution_bytes)
        if resolution["manifest_request_summary"] != {
            "npm": {
                "limit": 128,
                "matched": 131,
                "max_bytes": 262144,
                "oversized": 1,
                "requested": 128,
            },
            "python": {
                "limit": 32,
                "matched": 34,
                "max_bytes": 262144,
                "oversized": 0,
                "requested": 32,
            },
        }:
            raise AssertionError(
                f"manifest count/byte bounds are wrong: "
                f"{resolution['manifest_request_summary']!r}"
            )
        paths = [row["path"] for row in resolution["manifest_requests"]]
        if len(paths) != 160 or any(
            path.endswith("p128/package.json")
            or path.endswith("p129/package.json")
            or "oversized" in path
            or "symlink" in path
            for path in paths
        ):
            raise AssertionError(f"manifest cap admitted a forbidden path: {paths!r}")
        if not all(row["fulfilled"] for row in resolution["manifest_requests"]):
            raise AssertionError("host did not fulfill every bounded manifest request")
        if resolution["diagnostics"] != [
            {
                "bytes": 262145,
                "code": "discovery-manifest-oversized",
                "limit": 262144,
                "path": "packages/zz-oversized/package.json",
                "severity": "warning",
            },
            {
                "candidates": 130,
                "code": "discovery-manifest-candidates-truncated",
                "limit": 128,
                "path": "packages/p128/package.json",
                "severity": "warning",
            },
            {
                "candidates": 34,
                "code": "discovery-manifest-candidates-truncated",
                "limit": 32,
                "path": "python/p032/pyproject.toml",
                "severity": "warning",
            },
        ]:
            raise AssertionError(
                f"manifest bound diagnostics are wrong: {resolution['diagnostics']!r}"
            )
    finally:
        shutil.rmtree(fixture, ignore_errors=True)


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

    native_module = sys.modules["archbird._native"]
    check_native_manifest_protocol(native_module)
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
    monorepo_fixture = repository / "test/fixtures/zero_config_monorepo"
    monorepo_resolution = resolve_discovery(monorepo_fixture)
    monorepo_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(monorepo_fixture),
            "default",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if (
        bytes.fromhex(monorepo_completed.stdout.decode().strip())
        != monorepo_resolution
    ):
        raise AssertionError("Python and Node monorepo resolution artifacts differ")
    monorepo = json.loads(monorepo_resolution)
    if monorepo["project"] != "fixture":
        raise AssertionError(f"monorepo project identity is wrong: {monorepo!r}")
    packages = {
        package.get("identity"): package
        for package in monorepo["effective_config"]["packages"]
    }
    if set(packages) != {
        "fixture",
        "@fixture/core",
        "@fixture/types",
        "fixture-py",
        "fixture-worker",
    }:
        raise AssertionError(f"bounded package discovery is wrong: {packages!r}")
    if packages["@fixture/types"]["layer"] != "auto-typescript":
        raise AssertionError("TypeScript-only workspace package used the JS layer")
    python_layer = next(
        layer
        for layer in monorepo["effective_config"]["layers"]
        if layer["name"] == "auto-python"
    )
    if python_layer.get("import_roots") != ["py", "python/worker/src"]:
        raise AssertionError(
            f"manifest-backed Python roots are wrong: {python_layer!r}"
        )
    requested_paths = [row["path"] for row in monorepo["manifest_requests"]]
    if requested_paths != [
        "experiments/pyproject.toml",
        "packages/core/package.json",
        "packages/types/package.json",
        "py/pyproject.toml",
        "python/worker/pyproject.toml",
    ]:
        raise AssertionError(f"manifest request ledger is wrong: {requested_paths!r}")
    if any(
        "ignored" in path or "excluded" in path for path in requested_paths
    ):
        raise AssertionError("ignored/excluded workspace manifest was requested")
    if monorepo["manifest_request_summary"] != {
        "npm": {
            "limit": 128,
            "matched": 2,
            "max_bytes": 262144,
            "oversized": 0,
            "requested": 2,
        },
        "python": {
            "limit": 32,
            "matched": 3,
            "max_bytes": 262144,
            "oversized": 0,
            "requested": 3,
        },
    }:
        raise AssertionError(
            "manifest request bounds changed: "
            f"{monorepo['manifest_request_summary']!r}"
        )
    if monorepo["diagnostics"] != [
        {
            "code": "discovery-manifest-pattern-overlap",
            "limit": 1,
            "matches": 2,
            "path": "packages/core/package.json",
            "severity": "warning",
        }
    ]:
        raise AssertionError(
            f"workspace overlap evidence is wrong: {monorepo['diagnostics']!r}"
        )
    authored_config = canonical(
        {
            "layers": [
                {
                    "globs": ["**/*.js"],
                    "language": "javascript",
                    "name": "authored",
                }
            ],
            "packages": [
                {
                    "kind": "npm",
                    "layer": "authored",
                    "name": "authored",
                    "path": "package.json",
                }
            ],
            "project": "authored",
        }
    )
    authored_resolution = resolve_discovery(
        monorepo_fixture, config=authored_config
    )
    authored_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(monorepo_fixture),
            "authored",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if bytes.fromhex(authored_completed.stdout.decode().strip()) != authored_resolution:
        raise AssertionError("Python and Node authored-config artifacts differ")
    authored = json.loads(authored_resolution)["effective_config"]
    if authored["project"] != "authored" or authored["layers"] != [
        {
            "globs": ["**/*.js"],
            "language": "javascript",
            "name": "authored",
        }
    ] or authored["packages"] != [
        {
            "kind": "npm",
            "layer": "authored",
            "name": "authored",
            "path": "package.json",
        }
    ]:
        raise AssertionError(
            f"manifest candidates overrode authored architecture: {authored!r}"
        )
    ignore_overlay = {".archbirdignore": b"# virtual after-state\npackages/ignored/\n"}
    overlaid_resolution = resolve_discovery(
        monorepo_fixture, _source_overlay=ignore_overlay
    )
    overlaid_completed = subprocess.run(
        [
            str(node),
            str(repository / "test/test_resolution_node.js"),
            str(addon.resolve()),
            str(repository),
            str(monorepo_fixture),
            "ignore-overlay",
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    if (
        bytes.fromhex(overlaid_completed.stdout.decode().strip())
        != overlaid_resolution
    ):
        raise AssertionError("Python and Node overlaid-ignore artifacts differ")
    if any(
        "ignored" in request["path"]
        for request in json.loads(overlaid_resolution)["manifest_requests"]
    ):
        raise AssertionError("overlaid ignore rules did not protect manifest reads")
    check_bounded_manifest_discovery(
        resolve_discovery, node, addon, repository
    )
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
        "Python/Node config-resolution parity passed for bounded npm/Python "
        "monorepos, CRAN, Autoconf, SCIP, and compile_commands"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
