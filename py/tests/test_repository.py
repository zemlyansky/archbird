#!/usr/bin/env python3
"""End-to-end CPython host repository discovery and native Map smoke test."""

from __future__ import annotations

import copy
import importlib.util
import errno
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from unittest import mock


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_python_repository.py EXTENSION REPOSITORY_ROOT")
    repository = Path(sys.argv[2]).resolve()
    sys.path.insert(0, str(repository))
    import archbird
    from archbird.errors import ConfigError

    load_extension(Path(sys.argv[1]).resolve())
    from archbird import _native
    from archbird.native import (
        Project,
        Source,
        Workspace,
        analyze_okf_source,
        diff_maps_json,
        evaluate_constraints_json,
        export_graph,
        publish_okf_bundle,
        query_map_json,
        resolve_discovery,
        write_okf_bundle,
    )
    from archbird.provider_cache import (
        ProviderCache,
        default_provider_cache_max_bytes,
    )
    import archbird.provider_cache as provider_cache_module

    if len(_native.IMPLEMENTATION_SHA256) != 64 or any(
        character not in "0123456789abcdef"
        for character in _native.IMPLEMENTATION_SHA256
    ):
        raise AssertionError("native core implementation identity is invalid")

    conflict_project = Project(
        "merge-conflict",
        [Source("src/a.txt", b"abc", language="text")],
    )

    def conflicting_provider(provider_name: str, fact_name: str) -> bytes:
        return json.dumps(
            {
                "artifact": "archbird-provider-facts",
                "capabilities": [
                    {
                        "claims": ["syntax-structure"],
                        "coverage": "complete",
                        "domain": "symbols",
                    }
                ],
                "diagnostics": [],
                "facts": [
                    {
                        "claim": "syntax-structure",
                        "domain": "symbols",
                        "id": f"symbol:{fact_name}",
                        "key": "a",
                        "kind": "variable",
                        "name": fact_name,
                        "path": "src/a.txt",
                        "project": "merge-conflict",
                        "span": {"end": 3, "start": 0},
                    }
                ],
                "inputs": [
                    {
                        "project": "merge-conflict",
                        "source_manifest_sha256": conflict_project.manifest_sha256,
                    }
                ],
                "producer": {
                    "configuration_sha256": ("1" if provider_name == "primary" else "2")
                    * 64,
                    "implementation_sha256": "3" * 64,
                    "name": f"fixture-{provider_name}",
                    "version": "1",
                },
                "provenance": "derived",
                "resolutions": [],
                "schema_version": 1,
                "subject": {
                    "path": "src/a.txt",
                    "project": "merge-conflict",
                    "scope": "file",
                },
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()

    conflict_project.add_provider(conflicting_provider("primary", "a"), "primary")
    conflict_project.add_provider(conflicting_provider("augment", "b"), "augment")
    try:
        conflict_project.finalize_providers()
    except RuntimeError as error:
        compact_conflicts = json.loads(error.merge_conflicts_json)
    else:
        raise AssertionError("conflicting providers unexpectedly finalized")
    if compact_conflicts[
        "artifact"
    ] != "archbird-provider-merge-conflicts" or compact_conflicts["summary"] != {
        "conflicts": 1,
        "providers_in_conflicts": 2,
        "providers_total": 2,
    }:
        raise AssertionError(
            f"compact provider conflict evidence is incomplete: {compact_conflicts!r}"
        )
    if [
        compact_conflicts["conflicts"][0][side]["name"]
        for side in ("left_fact", "right_fact")
    ] != ["a", "b"]:
        raise AssertionError("compact provider conflict facts are incomplete")

    fixture = repository / "test/fixtures/map_base"
    project = Project.from_config(fixture / "archbird.json", root=fixture)
    paths = tuple(source.path for source in project.sources)
    if paths != (
        "js/index.js",
        "py/pkg/__init__.py",
        "py/pkg/api.py",
    ):
        raise AssertionError(f"unexpected discovered sources: {paths!r}")
    first = project.map_json()
    second = project.map_json()
    if first != second:
        raise AssertionError("repository Map output is not repeatable")
    streamed = io.BytesIO()
    project.write_map_json(streamed.write)
    if streamed.getvalue() != first:
        raise AssertionError("streaming changed canonical Map output")
    try:
        project.write_map_json(lambda _chunk: 0)
    except OSError:
        pass
    else:
        raise AssertionError("streaming accepted a short output write")
    if not any(
        project.provider_facts(index)["producer"]["name"].startswith(
            "archbird-tree-sitter-"
        )
        for index in range(project.counts["providers"])
    ):
        raise AssertionError("Python host did not expose portable syntax facts")
    parallel = Project.from_config(fixture / "archbird.json", root=fixture, jobs=2)
    if parallel.map_json() != first:
        raise AssertionError("Python process count changed native Map output")
    cache_root = repository / "build/test-provider-cache-python"
    shutil.rmtree(cache_root, ignore_errors=True)
    cached_cold = Project.from_config(
        fixture / "archbird.json",
        root=fixture,
        cache_dir=cache_root,
        map_cache=False,
    )
    cached_warm = Project.from_config(
        fixture / "archbird.json",
        root=fixture,
        cache_dir=cache_root,
        map_cache=False,
    )
    if cached_cold.map_json() != first or cached_warm.map_json() != first:
        raise AssertionError("Python provider caching changed canonical Map bytes")
    if (
        cached_cold.cache_stats["misses"] == 0
        or cached_cold.cache_stats["writes"] != cached_cold.cache_stats["misses"]
        or cached_warm.cache_stats["hits"] != cached_cold.cache_stats["writes"]
        or cached_warm.cache_stats["misses"] != 0
    ):
        raise AssertionError(
            "Python provider cache cold/warm accounting is incomplete: "
            f"{cached_cold.cache_stats!r} -> {cached_warm.cache_stats!r}"
        )
    c_cache_root = repository / "build/test-c-input-cache-python"
    shutil.rmtree(c_cache_root, ignore_errors=True)

    def c_cache_sources(header: bytes, note: bytes) -> tuple[Source, ...]:
        return (
            Source(
                "include/api.h",
                header,
                language="c",
                layer="core",
                roles=("public-header", "source"),
            ),
            Source("notes/state.txt", note, language="text", layer="docs"),
            Source(
                "src/api.c",
                b"int api(void) { return 1; }\n",
                language="c",
                layer="core",
            ),
        )

    c_cold = Project("c-input-cache", c_cache_sources(b"int api(void);\n", b"before\n"))
    c_cold.scan(cache_dir=c_cache_root, map_cache=False)
    c_unrelated = Project(
        "c-input-cache", c_cache_sources(b"int api(void);\n", b"after\n")
    )
    c_unrelated.scan(cache_dir=c_cache_root, map_cache=False)
    if c_unrelated.cache_stats["hits"] != 4 or c_unrelated.cache_stats["misses"]:
        raise AssertionError(
            "unrelated source invalidated C provider input closure: "
            f"{c_cold.cache_stats!r} -> {c_unrelated.cache_stats!r}"
        )
    c_header_changed = Project(
        "c-input-cache",
        c_cache_sources(b"int api(void);\nint added(void);\n", b"after\n"),
    )
    c_header_changed.scan(cache_dir=c_cache_root, map_cache=False)
    if (
        c_header_changed.cache_stats["hits"] != 1
        or c_header_changed.cache_stats["misses"] != 3
        or c_header_changed.cache_stats["invalid"] != 1
    ):
        raise AssertionError(
            "public-header change did not invalidate dependent C facts: "
            f"{c_header_changed.cache_stats!r}"
        )
    shutil.rmtree(c_cache_root, ignore_errors=True)
    map_cache_root = repository / "build/test-map-cache-python"
    shutil.rmtree(map_cache_root, ignore_errors=True)
    map_cold = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    map_cold_json = map_cold.map_json()
    map_warm = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    if map_warm.map_json() != map_cold_json:
        raise AssertionError("Python complete Map cache changed canonical bytes")
    if map_cold.map_cache_stats != {
        "errors": 0,
        "hits": 0,
        "invalid": 0,
        "misses": 1,
        "no_space": 0,
        "skipped": 0,
        "writes": 1,
    } or map_warm.map_cache_stats != {
        "errors": 0,
        "hits": 1,
        "invalid": 0,
        "misses": 0,
        "no_space": 0,
        "skipped": 0,
        "writes": 0,
    }:
        raise AssertionError(
            "Python complete Map cache accounting is incomplete: "
            f"{map_cold.map_cache_stats!r} -> {map_warm.map_cache_stats!r}"
        )
    if map_warm.cache_stats["hits"] != 0:
        raise AssertionError("Map hit unnecessarily loaded provider cache entries")
    if map_warm.counts["providers"] == 0:
        raise AssertionError("Map hit could not lazily materialize provider facts")
    map_files = tuple((map_cache_root / "maps-v1").rglob("*.json"))
    if len(map_files) != 1:
        raise AssertionError("Python complete Map cache path is not singular")
    map_files[0].write_bytes(b"{broken")
    map_recovered = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    if (
        map_recovered.map_json() != map_cold_json
        or map_recovered.map_cache_stats["invalid"] != 1
    ):
        raise AssertionError("Python complete Map cache did not reject corruption")
    changed_fixture = repository / "build/test-map-cache-source-python"
    shutil.rmtree(changed_fixture, ignore_errors=True)
    shutil.copytree(fixture, changed_fixture)
    changed_map = Project.from_config(
        changed_fixture / "archbird.json",
        root=changed_fixture,
        cache_dir=map_cache_root,
    )
    changed_map_json = changed_map.map_json()
    changed_source = changed_fixture / "py/pkg/api.py"
    changed_source.write_bytes(
        changed_source.read_bytes()
        + b"\ndef cache_invalidation_probe():\n    return 2\n"
    )
    changed_map_again = Project.from_config(
        changed_fixture / "archbird.json",
        root=changed_fixture,
        cache_dir=map_cache_root,
    )
    if (
        changed_map_again.map_cache_stats["misses"] != 1
        or changed_map_again.map_json() == changed_map_json
    ):
        raise AssertionError("changed Python source reused a stale complete Map")
    shutil.rmtree(changed_fixture, ignore_errors=True)
    shutil.rmtree(map_cache_root, ignore_errors=True)
    bounded_root = repository / "build/test-provider-cache-bounded-python"
    shutil.rmtree(bounded_root, ignore_errors=True)
    for invalid_budget in (True, 0, -1, 1.5, (1 << 53)):
        try:
            ProviderCache(bounded_root, max_bytes=invalid_budget)
        except ValueError:
            pass
        else:
            raise AssertionError(
                f"Python cache accepted invalid budget {invalid_budget!r}"
            )
    for invalid_environment in ("", "+1", " 1", "1.5", str(1 << 53)):
        with mock.patch.dict(
            os.environ,
            {"ARCHBIRD_CACHE_MAX_BYTES": invalid_environment},
        ):
            try:
                default_provider_cache_max_bytes()
            except ValueError:
                pass
            else:
                raise AssertionError(
                    "Python cache accepted invalid environment budget "
                    f"{invalid_environment!r}"
                )
    bounded = ProviderCache(bounded_root, max_bytes=100)
    cache_parameters = {
        "namespace": "fixture",
        "project": "cache-budget",
        "provider_id": "fixture",
        "path": "a.py",
        "source_sha256": "1" * 64,
    }
    bounded.store(b"a" * 60, **cache_parameters)
    second_parameters = dict(cache_parameters, path="b.py", source_sha256="2" * 64)
    bounded.store(b"b" * 60, **second_parameters)
    if bounded.stats.bytes > 100 or bounded.stats.evictions != 1:
        raise AssertionError(f"Python cache budget was not enforced: {bounded.stats}")
    if bounded.load(**cache_parameters) is not None:
        raise AssertionError("Python cache retained an evicted entry")
    if bounded.load(**second_parameters) != b"b" * 60:
        raise AssertionError("Python cache evicted the wrong entry")
    bounded.store(b"c" * 101, **cache_parameters)
    if bounded.stats.skipped != 1:
        raise AssertionError("Python cache did not reject an oversized entry")
    stale = bounded_root / "providers-v1" / "aa" / ".stale.tmp"
    stale.parent.mkdir(parents=True, exist_ok=True)
    stale.write_bytes(b"partial")
    recovered = ProviderCache(bounded_root, max_bytes=100)
    if stale.exists() or recovered.stats.temporaries_removed != 1:
        raise AssertionError("Python cache did not remove a failed temporary")
    with mock.patch.object(
        provider_cache_module.tempfile,
        "NamedTemporaryFile",
        side_effect=OSError(errno.ENOSPC, "no space left on device"),
    ):
        recovered.store(b"d", **cache_parameters)
    if recovered.stats.no_space != 1 or recovered.stats.errors != 1:
        raise AssertionError("Python cache did not classify ENOSPC")
    shutil.rmtree(bounded_root, ignore_errors=True)
    changed = Project(
        "cache-source",
        [Source("src/a.py", b"def a():\n    return 1\n", language="python")],
    )
    changed.scan(cache_dir=cache_root, map_cache=False)
    changed_repeat = Project(
        "cache-source",
        [Source("src/a.py", b"def a():\n    return 2\n", language="python")],
    )
    changed_repeat.scan(cache_dir=cache_root, map_cache=False)
    if (
        changed_repeat.cache_stats["hits"] != 0
        or changed_repeat.cache_stats["misses"] != 3
    ):
        raise AssertionError(
            "changed Python source reused stale provider facts: "
            f"{changed_repeat.cache_stats!r}"
        )
    shutil.rmtree(cache_root, ignore_errors=True)
    try:
        Project.from_config(fixture / "archbird.json", root=fixture, jobs=-1)
    except ValueError:
        pass
    else:
        raise AssertionError("negative Python process count was accepted")
    document = json.loads(first)
    if document["project"] != "map-base" or len(document["files"]) != 3:
        raise AssertionError("repository Map does not describe the fixture")

    zero_fixture = repository / "test/fixtures/zero_config"
    zero_resolution_json = resolve_discovery(zero_fixture)
    if zero_resolution_json != resolve_discovery(zero_fixture):
        raise AssertionError("config-free resolution is not repeatable")
    zero_resolution = json.loads(zero_resolution_json)
    if zero_resolution["project"] != "zero-fixture":
        raise AssertionError("scoped npm identity was not normalized as a project ID")
    expected_zero_paths = [
        "Makefile",
        "generated/parser.c",
        "large.py",
        "nested/keep.py",
        "package.json",
        "pyproject.toml",
        "src/custom.py",
        "src/main.js",
        "src/main.py",
        "src/reinclude.skip.py",
        "src/zero_python/__init__.py",
        "tests/test_main.py",
        "vendor/lib.c",
    ]
    if [row["path"] for row in zero_resolution["files"]] != expected_zero_paths:
        raise AssertionError("config-free source/ignore selection changed")
    roles = {row["path"]: row["roles"] for row in zero_resolution["files"]}
    if roles["tests/test_main.py"] != ["source", "test-candidate"]:
        raise AssertionError("config-free test evidence became an assertion")
    if roles["generated/parser.c"] != ["generated-candidate", "source"]:
        raise AssertionError("generated candidate evidence is missing")
    if roles["vendor/lib.c"] != ["source", "third-party-candidate"]:
        raise AssertionError("third-party candidate evidence is missing")
    if [row["path"] for row in zero_resolution["ignore_files"]] != [
        ".gitignore",
        ".ignore",
        ".archbirdignore",
        "nested/.gitignore",
    ]:
        raise AssertionError("ignored parents leaked nested ignore evidence")
    bounded = json.loads(
        resolve_discovery(
            zero_fixture,
            project="cli",
            ignore_files=(".customignore",),
            max_file_bytes=100,
        )
    )
    if bounded["project"] != "cli" or bounded["coverage"] != {
        "assets": 10,
        "ignored": 3,
        "inventory_files": 23,
        "oversized": 1,
        "pruned_directories": 1,
        "selected": 11,
        "unsupported_known": 1,
    }:
        raise AssertionError("config-free coverage or CLI precedence changed")
    if bounded["diagnostics"] != [
        {
            "bytes": 167,
            "code": "discovery-file-oversized",
            "limit": 100,
            "path": "large.py",
            "severity": "warning",
        }
    ]:
        raise AssertionError("oversized source evidence is incomplete")
    custom_only = json.loads(
        resolve_discovery(
            zero_fixture,
            ignore=False,
            ignore_files=(".customignore",),
        )
    )
    custom_only_paths = {row["path"] for row in custom_only["files"]}
    if "src/custom.py" in custom_only_paths or not {
        "ignored/drop.py",
        "nested/local.py",
        "src/from-ignore.py",
    }.issubset(custom_only_paths):
        raise AssertionError("--no-ignore did not reset only repository ignores")
    if [row["path"] for row in custom_only["ignore_files"]] != [
        ".customignore"
    ] or not any(
        row["pointer"] == "/selection/ignore" and row["source"] == "cli"
        for row in custom_only["origins"]
    ):
        raise AssertionError("custom-only ignore origin evidence is incomplete")
    zero_project = Project.from_repository(zero_fixture)
    zero_map = json.loads(zero_project.map_json())
    if zero_map["project"] != "zero-fixture":
        raise AssertionError("config-free Project failed to build a Map")
    packages = {row["name"]: row for row in zero_map["packages"]}
    if packages["npm-root"]["identity"] != "@archbird/zero-fixture":
        raise AssertionError("scoped npm package identity was lost")
    python_package = packages["python-root"]
    if (
        python_package["identity"] != "zero-python"
        or python_package["aliases"] != ["zero-python", "zero_python"]
        or python_package["entrypoints"]
        != {"configured:0": "src/zero_python/__init__.py"}
        or "answer" not in python_package["exports"]
    ):
        raise AssertionError(
            f"zero-config Python package is incomplete: {python_package!r}"
        )
    if not any(
        edge["kind"] == "imported-call"
        and edge["source"] == "tests/test_main.py"
        and edge["target"] == "src/zero_python/__init__.py"
        and edge["names"] == ["answer"]
        for edge in zero_map["edges"]
    ):
        raise AssertionError("zero-config Python package import was not resolved")
    if len(zero_map["tests"]) != 1:
        raise AssertionError(
            f"zero-config candidate test inventory is incomplete: {zero_map['tests']!r}"
        )
    candidate_test = zero_map["tests"][0]
    if (
        candidate_test["path"] != "tests/test_main.py"
        or candidate_test["inventory_source"] != "discovery"
        or candidate_test["inventory_state"] != "candidate"
        or [
            (case["selector"], case["evidence_kind"])
            for case in candidate_test["cases"]
        ]
        != [("test_main", "test_definition")]
        or candidate_test["cases"][0]["routes"] != {"src/zero_python/__init__.py": 1}
    ):
        raise AssertionError(
            f"zero-config test candidates became inaccurate: {candidate_test!r}"
        )
    if zero_map.get("discovery") != {
        "coverage": zero_resolution["coverage"],
        "profile": zero_resolution["profile"],
        "sha256": zero_resolution["sha256"],
    }:
        raise AssertionError("config-free Map lost discovery coverage evidence")
    c_registry_fixture = repository / "test/fixtures/zero_config_c_registry"
    c_registry_map = json.loads(Project.from_repository(c_registry_fixture).map_json())
    c_registry_test = next(
        row for row in c_registry_map["tests"] if row["path"] == "test/test_widget.c"
    )
    c_registry_cases = {row["selector"]: row for row in c_registry_test["cases"]}
    if sorted(c_registry_cases) != [
        "direct",
        "widget/explicit",
        "widget/forwarded",
    ]:
        raise AssertionError(
            f"zero-config C registry cases are incomplete: {c_registry_cases!r}"
        )
    if (
        c_registry_cases["direct"]["evidence_kind"] != "test_definition"
        or c_registry_cases["widget/explicit"]["evidence_kind"]
        != "test_registration_candidate"
        or c_registry_cases["widget/forwarded"]["evidence_kind"]
        != "test_registration_candidate"
        or c_registry_cases["widget/explicit"]["routes"] != {"test/test_widget.c": 1}
        or c_registry_cases["widget/forwarded"]["routes"] != {"test/test_widget.c": 1}
    ):
        raise AssertionError(
            f"zero-config C registry routes are inaccurate: {c_registry_cases!r}"
        )
    zero_report = zero_project.map_markdown(view="audit").decode("utf-8")
    if (
        "unsupported-known=1" not in zero_report
        or "Coverage warning:" not in zero_report
    ):
        raise AssertionError("Map Markdown hid unsupported-language coverage")
    if {row["path"] for row in zero_map["files"]} & {
        "ignored/drop.py",
        "nested/local.py",
        "src/from-ignore.py",
    }:
        raise AssertionError("ignored files entered canonical Map evidence")
    with tempfile.TemporaryDirectory(dir=repository / "build") as clone_directory:
        first_clone = Path(clone_directory) / "checkout-one"
        second_clone = Path(clone_directory) / "unrelated-folder-name"
        shutil.copytree(zero_fixture, first_clone)
        shutil.copytree(zero_fixture, second_clone)
        if resolve_discovery(first_clone) != resolve_discovery(second_clone):
            raise AssertionError("checkout directory name changed resolution evidence")
        clone_config = (first_clone / "archbird.json").read_bytes()
        first_clone_map = Project.from_repository(
            first_clone, config=clone_config
        ).map_json()
        second_clone_map = Project.from_repository(
            second_clone,
            config=(second_clone / "archbird.json").read_bytes(),
        ).map_json()
        if first_clone_map != second_clone_map:
            raise AssertionError("checkout path changed canonical Map evidence")
    standard_map = project.map_markdown()
    if not standard_map.startswith(b"# map-base architecture\n"):
        raise AssertionError("native Python standard Map report is invalid")
    if b"## Key files and symbols" not in standard_map:
        raise AssertionError("native Python overview omitted key files")
    if b"## Languages" not in project.map_markdown(view="architecture"):
        raise AssertionError("native Python architecture view omitted languages")
    if len(project.map_markdown(detail="compact")) >= len(standard_map):
        raise AssertionError("native Python compact detail was not compact")
    if project.map_markdown(full=True) == standard_map:
        raise AssertionError("native Python full Map report was not expanded")
    duplicate_call_document = json.loads(first)
    duplicate_call = dict(duplicate_call_document["call_resolutions"][0])
    duplicate_call["candidates"] = []
    duplicate_call["kind"] = "method"
    duplicate_call_document["call_resolutions"].append(duplicate_call)
    duplicate_call_json = json.dumps(
        duplicate_call_document,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    unchanged_diff = json.loads(
        diff_maps_json(duplicate_call_json, duplicate_call_json)
    )
    if any(unchanged_diff["sections"]["call_resolutions"].values()):
        raise AssertionError("native diff changed an identical grouped-call map")
    grouped_diff = json.loads(diff_maps_json(first, duplicate_call_json))
    changed_calls = grouped_diff["sections"]["call_resolutions"]["changed"]
    if len(changed_calls) != 1 or not changed_calls[0].startswith("js/index.js|add: "):
        raise AssertionError("native diff lost a legal same-name call resolution")
    file_scoped_document = json.loads(first)
    file_scoped_document["symbol_calls"].append(
        {
            "candidates": [{"path": "csrc/base.c", "symbol": "add"}],
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "file-call",
                    "line": 7,
                    "provider": "fixture",
                    "span": {"end": 30, "start": 24},
                }
            ],
            "name": "add",
            "resolution": "candidate",
            "source": {"path": "test.js", "scope": "test-file"},
        }
    )
    file_scoped_document["symbol_references"].append(
        {
            "candidates": [{"path": "csrc/base.c", "symbol": "add"}],
            "context": "identifier",
            "evidence": [
                {
                    "claim": "syntax-structure",
                    "fact_id": "file-reference",
                    "line": 8,
                    "provider": "fixture",
                    "span": {"end": 42, "start": 36},
                }
            ],
            "name": "add",
            "relation": "value",
            "resolution": "candidate",
            "source": {"path": "index.js", "scope": "file"},
        }
    )
    file_scoped_json = json.dumps(
        file_scoped_document,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    file_scoped_same = json.loads(diff_maps_json(file_scoped_json, file_scoped_json))
    if any(
        file_scoped_same["sections"][name][kind]
        for name in ("symbol_calls", "symbol_references")
        for kind in ("added", "changed", "removed")
    ):
        raise AssertionError("native diff changed identical file-scoped relations")
    file_scoped_changed = json.loads(file_scoped_json)
    file_scoped_changed["symbol_calls"][-1]["evidence"][0]["line"] = 9
    file_scoped_delta = json.loads(
        diff_maps_json(
            file_scoped_json,
            json.dumps(
                file_scoped_changed,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8"),
        )
    )
    if len(file_scoped_delta["sections"]["symbol_calls"]["changed"]) != 1:
        raise AssertionError("native diff lost a file-scoped relation change")
    graphml = export_graph(first, format="graphml", view="files")
    mermaid = export_graph(first, format="mermaid", view="components")
    graph_json = project.graph_view_json(view="components")
    symbol_graph_json = project.graph_view_json(
        view="symbols",
        query={"symbols": ["js/index.js:add"], "depth": 1, "test_depth": 1},
    )
    if not graphml.startswith(b"<?xml") or b"<node " not in graphml:
        raise AssertionError("native Python GraphML export is invalid")
    if not mermaid.startswith(b"%% Archbird components graph"):
        raise AssertionError("native Python Mermaid export is invalid")
    graph_document = json.loads(graph_json)
    symbol_graph_document = json.loads(symbol_graph_json)
    if (
        graph_document["artifact"] != "archbird-graph-view"
        or graph_document["request"]["view"] != "components"
        or graph_document["source"]["artifact"] != "map"
    ):
        raise AssertionError("native Python component graph JSON is invalid")
    if (
        symbol_graph_document["request"]["view"] != "symbols"
        or symbol_graph_document["source"]["artifact"] != "query"
        or not any(
            node["kind"] == "symbol" and node["label"] == "add"
            for node in symbol_graph_document["nodes"]
        )
    ):
        raise AssertionError("native Python symbol graph JSON is invalid")
    if symbol_graph_json != project.graph_view_json(
        view="symbols",
        query={"symbols": ["js/index.js:add"], "depth": 1, "test_depth": 1},
    ):
        raise AssertionError("native Python symbol graph JSON is not repeatable")
    okf_source = (repository / "test/fixtures/okf/source-bundle.json").read_bytes()
    okf_first = analyze_okf_source(okf_source)
    if okf_first != analyze_okf_source(okf_source):
        raise AssertionError("native Python OKF index is not repeatable")
    okf_document = json.loads(okf_first)
    if (
        okf_document["artifact"] != "okf-index"
        or okf_document["summary"]["concepts"] != 1
    ):
        raise AssertionError("native Python OKF index is invalid")
    try:
        export_graph(first, format="mermaid", view="files", max_nodes=1)
    except RuntimeError:
        pass
    else:
        raise AssertionError("Python Mermaid node limit was not enforced")
    query = project.query(paths=["py/pkg"], depth=0)
    if query["artifact"] != "query" or len(query["files"]) != 2:
        raise AssertionError("repository query did not select the package directory")
    retrieval_query = project.query(
        search=["twce python"], search_limit=4, depth=0, test_depth=0
    )
    retrieval = retrieval_query["query"]["retrieval"]
    if (
        retrieval["contract"] != "archbird-lexical-ranking-v2"
        or retrieval["confidence"] != "candidate"
        or len(retrieval["hits"]) != 4
        or retrieval["hits"][0]["path"] != "py/pkg/api.py"
        or retrieval["hits"][0]["name"] != "twice"
        or not any(
            reason["match"] == "edit-1" for reason in retrieval["hits"][0]["reasons"]
        )
    ):
        raise AssertionError(f"unexpected deterministic retrieval: {retrieval!r}")
    retrieval_markdown = project.query_markdown(
        search=["twce python"], search_limit=4, depth=0, test_depth=0
    )
    if (
        b"## Candidate seeds" not in retrieval_markdown
        or b"twice" not in retrieval_markdown
    ):
        raise AssertionError("retrieval Markdown omitted ranked candidate evidence")
    if retrieval_query != project.query(
        search=["twce python"], search_limit=4, depth=0, test_depth=0
    ):
        raise AssertionError("deterministic retrieval is not repeatable")
    report_map_json = (repository / "test/fixtures/report_map.json").read_bytes()
    package_retrieval = json.loads(
        query_map_json(
            report_map_json,
            search=["@sample dep"],
            search_limit=5,
            depth=0,
            test_depth=0,
        )
    )["query"]["retrieval"]
    if (
        package_retrieval["hits"][0]["kind"] != "package"
        or package_retrieval["hits"][0]["name"] != "sample"
        or not any(
            reason["field"] == "package.dependency" and reason["value"] == "@sample/dep"
            for reason in package_retrieval["hits"][0]["reasons"]
        )
    ):
        raise AssertionError(
            f"package metadata retrieval lost its witness: {package_retrieval!r}"
        )
    call_retrieval = json.loads(
        query_map_json(
            report_map_json,
            search=["Find Worker in the code"],
            search_limit=5,
            depth=0,
            test_depth=0,
        )
    )["query"]["retrieval"]
    if {term.lower() for term in call_retrieval["terms"]} & {"in", "the"}:
        raise AssertionError("multi-term retrieval retained common prose terms")
    if not any(
        hit.get("path") == "js/main.js"
        and any(
            reason["field"] == "file.call" and reason["value"] == "Worker"
            for reason in hit["reasons"]
        )
        for hit in call_retrieval["hits"]
    ):
        raise AssertionError(
            f"call-name retrieval lost its file witness: {call_retrieval!r}"
        )
    shorthand_map = json.loads(report_map_json)
    websocket_file = dict(shorthand_map["files"][0])
    websocket_file.update(
        {
            "call_counts": {},
            "calls": [],
            "exports": [],
            "imports": [],
            "language": "c",
            "layer": "core",
            "path": "ws.c",
            "symbols": [],
        }
    )
    decoder_file = dict(websocket_file)
    decoder_file["path"] = "codec.c"
    decoder_file["symbols"] = [
        {
            "kind": "class",
            "line": 1,
            "name": "ZStandardDecoder",
            "scope": "public",
            "signature": "class ZStandardDecoder",
        }
    ]
    shorthand_map["files"].extend((websocket_file, decoder_file))
    shorthand_map["files"].sort(key=lambda row: row["path"])
    for query, expected_path, expected_match in (
        ("WebSocket", "ws.c", "acronym"),
        ("zstd", "codec.c", "abbreviation"),
    ):
        shorthand = json.loads(
            query_map_json(
                json.dumps(
                    shorthand_map, sort_keys=True, separators=(",", ":")
                ).encode(),
                search=[query],
                search_limit=5,
                depth=0,
                test_depth=0,
            )
        )["query"]["retrieval"]
        if not any(
            hit.get("path") == expected_path
            and any(reason["match"] == expected_match for reason in hit["reasons"])
            for hit in shorthand["hits"]
        ):
            raise AssertionError(
                f"retrieval lost {expected_match} witness for {query}: {shorthand!r}"
            )
    route_ranking_map = copy.deepcopy(shorthand_map)
    primary = next(row for row in route_ranking_map["files"] if row["path"] == "ws.c")
    primary["path"] = "src/session.c"
    primary["symbols"] = [
        {
            "kind": "function",
            "line": 3,
            "name": "WebSocketSession",
            "scope": "global",
            "signature": "void WebSocketSession(void)",
        }
    ]
    noise = next(row for row in route_ranking_map["files"] if row["path"] == "codec.c")
    noise["path"] = "src/memory.c"
    noise["symbols"] = [
        {
            "kind": "function",
            "line": 7,
            "name": "free",
            "scope": "global",
            "signature": "void free(void *)",
        }
    ]
    route_ranking_map["files"].sort(key=lambda row: row["path"])

    def route_evidence(
        *, fact_id: str, relation: str, target: str, target_symbol: str
    ) -> dict[str, object]:
        return {
            "claim": "syntax-structure",
            "fact_id": fact_id,
            "line": 1,
            "provenance": "derived",
            "provider": "fixture",
            "relation": relation,
            "scope": "case",
            "span": {"end": 1, "start": 0},
            "target": target,
            "target_symbol": target_symbol,
        }

    route_ranking_map["tests"] = [
        {
            "cases": [
                {
                    "configured_routes": [],
                    "line": 10,
                    "route_evidence": [
                        route_evidence(
                            fact_id="strong-candidate",
                            relation="call-candidate",
                            target="src/session.c",
                            target_symbol="WebSocketSession",
                        )
                    ],
                    "routes": {"src/session.c": 1},
                    "selector": "websocket session survives early cleanup",
                },
                {
                    "configured_routes": [],
                    "line": 20,
                    "route_evidence": [
                        route_evidence(
                            fact_id="weak-direct",
                            relation="call",
                            target="src/memory.c",
                            target_symbol="free",
                        )
                    ],
                    "routes": {"src/memory.c": 1},
                    "selector": "unrelated allocator smoke",
                },
            ],
            "count": 2,
            "framework": "fixture",
            "group": "fixture",
            "path": "test/routes.c",
            "routes": {"src/memory.c": 1, "src/session.c": 1},
        }
    ]
    route_ranked = json.loads(
        query_map_json(
            json.dumps(
                route_ranking_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            search=["WebSocket session early free"],
            search_limit=100,
            depth=0,
            test_depth=0,
        )
    )
    ranked_matches = route_ranked["test_matches"]
    if (
        len(ranked_matches) != 2
        or ranked_matches[0]["selector"] != "websocket session survives early cleanup"
        or ranked_matches[0]["classification"] != "candidate"
        or ranked_matches[1]["classification"] != "direct"
        or ranked_matches[0]["seed_retrieval_score"]
        <= ranked_matches[1]["seed_retrieval_score"]
        or ranked_matches[0]["seed_retrieval_rank"]
        >= ranked_matches[1]["seed_retrieval_rank"]
        or ranked_matches[0]["ranking_affinity"] == 0
    ):
        raise AssertionError(
            f"test ranking discarded retrieval-seed relevance: {ranked_matches!r}"
        )
    symbol_hop_map = copy.deepcopy(route_ranking_map)
    symbol_hop_map["tests"] = [
        {
            "cases": [
                {
                    "configured_routes": [],
                    "line": 30,
                    "route_evidence": [
                        route_evidence(
                            fact_id="related-selected-symbol",
                            relation="call",
                            target="src/memory.c",
                            target_symbol="free",
                        )
                    ],
                    "routes": {"src/memory.c": 1},
                    "selector": "replay symbolic input view",
                },
                {
                    "configured_routes": [],
                    "line": 35,
                    "route_evidence": [
                        route_evidence(
                            fact_id="related-selected-symbol-generic",
                            relation="call",
                            target="src/memory.c",
                            target_symbol="free",
                        )
                    ],
                    "routes": {"src/memory.c": 1},
                    "selector": "prepare operation",
                },
                {
                    "configured_routes": [],
                    "line": 40,
                    "route_evidence": [
                        route_evidence(
                            fact_id="related-unselected-symbol",
                            relation="call",
                            target="src/memory.c",
                            target_symbol="allocate",
                        )
                    ],
                    "routes": {"src/memory.c": 1},
                    "selector": "unrelated allocator path",
                },
            ],
            "count": 3,
            "framework": "fixture",
            "group": "fixture",
            "path": "test/symbol_routes.c",
            "routes": {"src/memory.c": 3},
        }
    ]
    symbol_hop_primary = next(
        row for row in symbol_hop_map["files"] if row["path"] == "src/session.c"
    )
    symbol_hop_primary["symbols"][0]["name"] = "prepare_input_view"
    symbol_hop_primary["symbols"][0]["signature"] = (
        "void prepare_input_view(void)"
    )
    symbol_hop_noise = next(
        row for row in symbol_hop_map["files"] if row["path"] == "src/memory.c"
    )
    symbol_hop_noise["symbols"].append(
        {
            "kind": "function",
            "line": 9,
            "name": "allocate",
            "scope": "global",
            "signature": "void *allocate(void)",
        }
    )
    final_file = copy.deepcopy(primary)
    final_file["path"] = "src/final.c"
    final_file["symbols"] = [
        {
            "kind": "function",
            "line": 11,
            "name": "finalize_session",
            "scope": "global",
            "signature": "void finalize_session(void)",
        }
    ]
    symbol_hop_map["files"].append(final_file)
    symbol_hop_map["files"].sort(key=lambda row: row["path"])

    def symbol_call(
        source_path: str,
        source_symbol: str,
        target_path: str,
        target_symbol: str,
    ) -> dict[str, object]:
        return {
            "candidates": [{"line": 1, "path": target_path, "symbol": target_symbol}],
            "evidence": [],
            "name": target_symbol,
            "resolution": "unique",
            "source": {"path": source_path, "symbol": source_symbol},
        }

    symbol_hop_map["symbol_calls"] = [
        symbol_call("src/session.c", "prepare_input_view", "src/memory.c", "free"),
        symbol_call("src/memory.c", "free", "src/final.c", "finalize_session"),
    ]
    symbol_hop_map["symbol_references"] = []
    symbol_hop_bytes = json.dumps(
        symbol_hop_map, sort_keys=True, separators=(",", ":")
    ).encode()
    symbol_hop_files = []
    for depth in (0, 1, 2):
        symbol_hop_files.append(
            {
                row["path"]
                for row in json.loads(
                    query_map_json(
                        symbol_hop_bytes,
                        symbols=["src/session.c:prepare_input_view"],
                        depth=depth,
                        test_depth=0,
                    )
                )["files"]
            }
        )
    if symbol_hop_files != [
        {"src/session.c"},
        {"src/memory.c", "src/session.c"},
        {"src/final.c", "src/memory.c", "src/session.c"},
    ]:
        raise AssertionError(
            f"symbol relation traversal ignored the requested depth: {symbol_hop_files!r}"
        )
    symbol_route_query = json.loads(
        query_map_json(
            symbol_hop_bytes,
            symbols=["src/session.c:prepare_input_view"],
            depth=1,
            test_depth=1,
        )
    )
    symbol_routes = {
        row["selector"]: row for row in symbol_route_query["test_matches"]
    }
    selected_route = symbol_routes["replay symbolic input view"]
    unrelated_route = symbol_routes["unrelated allocator path"]
    if {
        key: selected_route[key]
        for key in (
            "classification",
            "confidence",
            "evidence_scope",
            "seed_distance",
            "symbol_distance",
            "target_role",
            "target",
        )
    } != {
        "classification": "direct",
        "confidence": "exact",
        "evidence_scope": "case",
        "seed_distance": 1,
        "symbol_distance": 1,
        "target_role": "symbol-neighborhood",
        "target": {"path": "src/memory.c", "symbol": "free"},
    }:
        raise AssertionError(
            f"related symbol route lost exact membership: {selected_route!r}"
        )
    if (
        unrelated_route["classification"] != "conservative"
        or unrelated_route["confidence"] != "conservative"
        or unrelated_route["evidence_scope"] != "case"
        or unrelated_route["symbol_distance"] is not None
        or unrelated_route["target"] != {"path": "src/memory.c"}
    ):
        raise AssertionError(
            "an unselected same-file symbol became a selected route: "
            f"{unrelated_route!r}"
        )
    advisory_symbol_query = json.loads(
        query_map_json(
            symbol_hop_bytes,
            search=["prepare input view"],
            search_limit=1,
            depth=1,
            test_depth=1,
        )
    )
    advisory_routes = {
        row["selector"]: row for row in advisory_symbol_query["test_matches"]
    }
    advisory_route = advisory_routes["replay symbolic input view"]
    if (
        advisory_route["classification"] != "conservative"
        or advisory_route["confidence"] != "conservative"
        or advisory_route["symbol_distance"] is not None
    ):
        raise AssertionError(
            "an advisory retrieval seed strengthened a transitive test route: "
            f"{advisory_route!r}"
        )
    supported_legacy_map = json.loads(report_map_json)
    for package in supported_legacy_map["packages"]:
        package.pop("aliases", None)
        package.pop("dependencies", None)
        package.pop("exports", None)
    legacy_retrieval = json.loads(
        query_map_json(
            json.dumps(
                supported_legacy_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            search=["sample"],
            search_limit=1,
            depth=0,
            test_depth=0,
        )
    )["query"]["retrieval"]
    if legacy_retrieval["hits"][0]["kind"] != "package":
        raise AssertionError("retrieval rejected optional legacy package fields")
    malformed_package_map = json.loads(report_map_json)
    malformed_package_map["packages"][0]["aliases"] = {}
    try:
        query_map_json(
            json.dumps(
                malformed_package_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            search=["sample"],
            depth=0,
            test_depth=0,
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("retrieval accepted malformed package metadata")
    malformed_call_map = json.loads(report_map_json)
    malformed_call_map["call_resolutions"].reverse()
    try:
        query_map_json(
            json.dumps(
                malformed_call_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            search=["Worker"],
            depth=0,
            test_depth=0,
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("retrieval accepted unsorted call evidence")
    for invalid in (
        {"search": ["---"]},
        {"search": [" ".join(f"term{index}" for index in range(17))]},
        {"search": ["sample"], "search_limit": 0},
        {"search": ["sample"], "search_limit": 101},
    ):
        try:
            query_map_json(
                report_map_json,
                depth=0,
                test_depth=0,
                **invalid,
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"invalid retrieval was accepted: {invalid!r}")
    if not project.query_markdown(paths=["py/pkg"], depth=0).startswith(
        b"# Focused architecture map: map-base\n"
    ):
        raise AssertionError("native Python query Markdown is invalid")
    change_brief = project.query_markdown(paths=["py/pkg"], depth=0, view="changes")
    if not change_brief.startswith(b"# Change brief: map-base\n"):
        raise AssertionError("native Python change brief is invalid")
    for expected in (
        b"## Affected code",
        b"## Routes, tests, and delivery",
        b"## Evidence limits",
    ):
        if expected not in change_brief:
            raise AssertionError(f"Python change brief omitted {expected!r}")
    if change_brief != project.query_markdown(
        paths=["py/pkg"], depth=0, view="changes"
    ):
        raise AssertionError("Python change brief is not repeatable")
    for invalid_options in (
        {"view": "other"},
        {"detail": "other"},
        {"compact": True, "full": True},
        {"compact": True, "detail": "full"},
    ):
        try:
            project.query_markdown(paths=["py/pkg"], depth=0, **invalid_options)
        except ValueError:
            pass
        else:
            raise AssertionError(
                f"Python accepted invalid query projection: {invalid_options!r}"
            )
    context_policy = {"profile": "exact", "quotas": {"files": 1}}
    context_query = project.query(paths=["py/pkg"], depth=0, context=context_policy)
    if (
        context_query["query"]["context"] != context_policy
        or len(context_query["files"]) != 2
    ):
        raise AssertionError("Python context policy changed canonical Query facts")
    context_report = project.query_markdown(
        paths=["py/pkg"], depth=0, context=context_policy
    ).decode()
    if (
        "Context: profile=exact;" not in context_report
        or "files=1/2." not in context_report
        or "## Selection manifest" not in context_report
    ):
        raise AssertionError("Python context profile was not applied")
    if (
        project.config_sha256
        != json.loads(project.manifest_json)["configuration_sha256"]
    ):
        raise AssertionError("manifest and decoded config identities differ")
    published_okf = publish_okf_bundle(first)
    if published_okf != publish_okf_bundle(first):
        raise AssertionError("native Python OKF publication is not repeatable")
    published_document = json.loads(published_okf)
    if published_document["artifact"] != "okf-output-bundle":
        raise AssertionError("native Python OKF publication has wrong artifact")
    unicode_map = json.loads(first)
    unicode_map["project"] = "Straße"
    unicode_okf = json.loads(
        publish_okf_bundle(
            json.dumps(
                unicode_map,
                ensure_ascii=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        )
    )
    if unicode_okf["project"] != "Straße":
        raise AssertionError("native Python OKF normalization lost Unicode")
    with tempfile.TemporaryDirectory() as okf_directory:
        output = Path(okf_directory) / "bundle"
        write_okf_bundle(published_okf, output)
        if not (output / "provenance" / "integrity.md").is_file():
            raise AssertionError("native Python OKF installation lost integrity")
        write_okf_bundle(published_okf, output, replace=True)
        (output / "index.md").write_text("manual edit\n", encoding="utf-8")
        try:
            write_okf_bundle(published_okf, output, replace=True)
        except ConfigError:
            pass
        else:
            raise AssertionError("native Python OKF replaced a curated bundle")
    if archbird.__version__ != "0.0.1":
        raise AssertionError("unexpected Python distribution version")
    if archbird.PATTERN_CONTRACT_VERSION != 1:
        raise AssertionError("unexpected configured-pattern contract version")
    if archbird.PATTERN_CONTRACT != "archbird-pcre2-v1":
        raise AssertionError("unexpected configured-pattern contract")
    if archbird.PATTERN_ENGINE != "PCRE2 10.47":
        raise AssertionError("unexpected configured-pattern engine")
    if archbird.PATTERN_UNICODE != "UCD 16.0.0":
        raise AssertionError("unexpected configured-pattern Unicode data")
    if archbird.PATTERN_OPTIONS != (
        "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,NEVER_CALLOUT,JIT_DISABLED"
    ):
        raise AssertionError("unexpected configured-pattern options")
    workspace = Workspace.from_config(repository / "test/fixtures/workspace.json")
    workspace_document = workspace.data()
    if workspace_document["workspace"] != "fixture-workspace":
        raise AssertionError("Python host did not load the workspace")
    if len(workspace_document["routes"]) != 2:
        raise AssertionError("Python host did not resolve workspace routes")
    self_config = (repository / "archbird.json").read_bytes()
    self_project = Project.from_repository(
        repository, config=self_config, jobs=1
    )
    self_map = self_project.map_json()
    first_verification = evaluate_constraints_json(
        self_config,
        self_map,
        resolution_json=self_project.resolution_json or b"",
    )
    if first_verification != evaluate_constraints_json(
        self_config,
        self_map,
        resolution_json=self_project.resolution_json or b"",
    ):
        raise AssertionError("Python constraint result is not repeatable")
    verification_document = json.loads(first_verification)
    if verification_document["artifact"] != "verification":
        raise AssertionError("Python host did not return verification evidence")
    if verification_document["summary"]["blocking"]:
        raise AssertionError(verification_document["constraints"])
    if any(
        row["status"] != "pass" for row in verification_document["constraints"]
    ):
        raise AssertionError("reviewed self constraints did not pass")
    from archbird.cli import main as cli_main

    with tempfile.TemporaryDirectory(dir=repository / "build") as directory:
        saved_map = Path(directory) / "map.json"
        saved_map.write_bytes(first)
        for query_command, selector in (
            ("query", ["--path", "py/pkg"]),
            ("impact", ["--symbol", "add"]),
        ):
            query_output = Path(directory) / f"{query_command}.json"
            status = cli_main(
                [
                    query_command,
                    "--map",
                    str(saved_map),
                    *selector,
                    "--format",
                    "json",
                    "--output",
                    str(query_output),
                ]
            )
            query_document = json.loads(query_output.read_bytes())
            if status or query_document["artifact"] != "query":
                raise AssertionError(f"native Python {query_command} CLI failed")
        brief_output = Path(directory) / "changes.md"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--path",
                "py/pkg",
                "--view",
                "changes",
                "--detail",
                "compact",
                "--output",
                str(brief_output),
            ]
        )
        brief_text = brief_output.read_text(encoding="utf-8")
        if status or not brief_text.startswith("# Change brief: map-base\n"):
            raise AssertionError("native Python changes view CLI failed")
        if "## Evidence limits" not in brief_text:
            raise AssertionError("native Python changes view lost omission accounting")
        retrieval_output = Path(directory) / "retrieval.json"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--search",
                "twce python",
                "--search-limit",
                "4",
                "--depth",
                "0",
                "--format",
                "json",
                "--output",
                str(retrieval_output),
            ]
        )
        retrieval_document = json.loads(retrieval_output.read_bytes())
        if (
            status
            or retrieval_document["query"]["retrieval"]["hits"][0]["name"] != "twice"
        ):
            raise AssertionError("native Python retrieval CLI failed")
        checked_query = Path(directory) / "checked-query.json"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--path",
                "py/pkg",
                "--format",
                "json",
                "--check",
                "--output",
                str(checked_query),
            ]
        )
        if status or json.loads(checked_query.read_bytes())["artifact"] != "query":
            raise AssertionError("current saved Map failed producer coherence")
        mismatched_map = Path(directory) / "mismatched-map.json"
        mismatched_document = json.loads(first)
        mismatched_document["tool"]["implementation_sha256"] = "0" * 64
        mismatched_map.write_text(
            json.dumps(mismatched_document, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
        )
        blocked_query = Path(directory) / "blocked-query.json"
        status = cli_main(
            [
                "query",
                "--map",
                str(mismatched_map),
                "--path",
                "py/pkg",
                "--format",
                "json",
                "--check",
                "--output",
                str(blocked_query),
            ]
        )
        if status != 1 or blocked_query.exists():
            raise AssertionError("saved Map producer mismatch was not blocked")
        missing_producer_map = Path(directory) / "missing-producer-map.json"
        missing_producer_document = json.loads(first)
        del missing_producer_document["tool"]["implementation_sha256"]
        missing_producer_map.write_text(
            json.dumps(
                missing_producer_document, sort_keys=True, separators=(",", ":")
            ),
            encoding="utf-8",
        )
        status = cli_main(
            [
                "query",
                "--map",
                str(missing_producer_map),
                "--path",
                "py/pkg",
                "--format",
                "json",
                "--check",
                "--output",
                str(blocked_query),
            ]
        )
        if status != 1 or blocked_query.exists():
            raise AssertionError("missing saved Map producer identity was not blocked")
        cross_version_query = Path(directory) / "cross-version-query.json"
        status = cli_main(
            [
                "query",
                "--map",
                str(mismatched_map),
                "--path",
                "py/pkg",
                "--format",
                "json",
                "--output",
                str(cross_version_query),
            ]
        )
        cross_version_document = json.loads(cross_version_query.read_bytes())
        if (
            status
            or cross_version_document["source_tool"]["implementation_sha256"]
            != "0" * 64
            or cross_version_document["tool"]["implementation_sha256"]
            != _native.IMPLEMENTATION_SHA256
        ):
            raise AssertionError(
                "plain cross-version saved Map query was not preserved"
            )
        qualified_output = Path(directory) / "qualified-query.json"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--symbol",
                "py/pkg/api.py:add",
                "--depth",
                "0",
                "--test-depth",
                "0",
                "--format",
                "json",
                "--output",
                str(qualified_output),
            ]
        )
        qualified_document = json.loads(qualified_output.read_bytes())
        if status or [
            (row["path"], row["name"]) for row in qualified_document["matched_symbols"]
        ] != [("py/pkg/api.py", "add")]:
            raise AssertionError("path-qualified Python CLI symbol selection failed")
        map_report = Path(directory) / "map.md"
        merge_conflicts_report = Path(directory) / "provider-conflicts.json"
        status = cli_main(
            [
                "--config",
                str(fixture / "archbird.json"),
                "--root",
                str(fixture),
                "--merge-ledger",
                str(merge_conflicts_report),
                "--output",
                str(map_report),
            ]
        )
        if status or not map_report.read_text(encoding="utf-8").startswith(
            "# map-base architecture\n"
        ):
            raise AssertionError("native Python default Map Markdown CLI failed")
        merge_conflicts = json.loads(merge_conflicts_report.read_bytes())
        if (
            merge_conflicts["artifact"] != "archbird-provider-merge-conflicts"
            or merge_conflicts["summary"]["conflicts"] != 0
        ):
            raise AssertionError("native Python CLI merge conflict ledger failed")
        zero_map_path = Path(directory) / "zero-map.json"
        status = cli_main(
            [
                "map",
                str(zero_fixture),
                "--no-config",
                "--format",
                "json",
                "--output",
                str(zero_map_path),
            ]
        )
        if (
            status
            or json.loads(zero_map_path.read_bytes())["project"] != "zero-fixture"
        ):
            raise AssertionError("native Python config-free Map CLI failed")
        configured_map_path = Path(directory) / "configured-map.json"
        status = cli_main(
            [
                "map",
                str(zero_fixture),
                "--project",
                "cli-fixture",
                "--format",
                "json",
                "--output",
                str(configured_map_path),
            ]
        )
        configured_map = json.loads(configured_map_path.read_bytes())
        if (
            status
            or configured_map["project"] != "cli-fixture"
            or any(row["path"] == "src/main.js" for row in configured_map["files"])
        ):
            raise AssertionError("CLI > config > discovery precedence failed")

        observation_fixture = repository / "test/fixtures/map_correctness"
        observation_project = Project.from_config(
            observation_fixture / "archbird.json", root=observation_fixture
        )
        evidence = [
            {
                "path": "test/test_cases.c",
                "role": "runner",
                "sha256": hashlib.sha256(
                    (observation_fixture / "test/test_cases.c").read_bytes()
                ).hexdigest(),
            },
            {
                "path": "csrc/callbacks.c",
                "role": "subject",
                "sha256": hashlib.sha256(
                    (observation_fixture / "csrc/callbacks.c").read_bytes()
                ).hexdigest(),
            },
            {
                "path": "test/test_cases.c",
                "role": "test_inventory",
                "sha256": hashlib.sha256(
                    (observation_fixture / "test/test_cases.c").read_bytes()
                ).hexdigest(),
            },
        ]

        def canonical(value: object) -> bytes:
            return json.dumps(
                value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
            ).encode("utf-8")

        observation = {
            "artifact": "archbird-test-symbol-observations",
            "cases": [
                {
                    "group": "c",
                    "path": "test/test_cases.c",
                    "selector": "sched.2d_e2e",
                    "symbols": [
                        {
                            "hits": 1,
                            "path": "csrc/callbacks.c",
                            "symbol": "alpha_callback",
                        }
                    ],
                }
            ],
            "producer": {
                "configuration_sha256": "4" * 64,
                "implementation_sha256": "3" * 64,
                "name": "python-cli-symbol-runner",
                "runtime": "fixture-runtime",
                "version": "1",
            },
            "project": "map-correctness",
            "provenance": "observed",
            "schema_version": 1,
            "source": {
                "config_sha256": observation_project.config_sha256,
                "evidence": evidence,
                "evidence_slice_sha256": hashlib.sha256(
                    canonical(evidence)
                ).hexdigest(),
                "map_input_sha256": observation_project.map_input_sha256,
            },
        }
        observation_path = Path(directory) / "test-symbol-observations.json"
        observation_path.write_bytes(canonical(observation))
        observed_query_path = Path(directory) / "observed-query.json"
        status = cli_main(
            [
                "query",
                "--root",
                str(observation_fixture),
                "--config",
                str(observation_fixture / "archbird.json"),
                "--test-symbol-observations",
                str(observation_path),
                "--symbol",
                "csrc/callbacks.c:alpha_callback",
                "--direction",
                "upstream",
                "--depth",
                "0",
                "--test-depth",
                "1",
                "--format",
                "json",
                "--output",
                str(observed_query_path),
            ]
        )
        observed_query = json.loads(observed_query_path.read_bytes())
        observed_match = next(
            row
            for row in observed_query["test_matches"]
            if row["group"] == "c"
            and row["path"] == "test/test_cases.c"
            and row["selector"] == "sched.2d_e2e"
        )
        if status or observed_match["classification"] != "observed":
            raise AssertionError("native Python observation CLI routing failed")
        resolution_path = Path(directory) / "resolution.json"
        status = cli_main(
            [
                "config",
                "show",
                str(zero_fixture),
                "--no-config",
                "--output",
                str(resolution_path),
            ]
        )
        if status or json.loads(resolution_path.read_bytes())["artifact"] != (
            "archbird-config-resolution"
        ):
            raise AssertionError("native Python config show failed")
        initialized = Path(directory) / "initialized.json"
        status = cli_main(
            [
                "config",
                "init",
                str(zero_fixture),
                "--no-config",
                "--output",
                str(initialized),
            ]
        )
        initialized_document = json.loads(initialized.read_bytes())
        if status or initialized_document["project"] != "zero-fixture":
            raise AssertionError("native Python config init failed")
        if (
            cli_main(
                [
                    "config",
                    "init",
                    str(zero_fixture),
                    "--no-config",
                    "--output",
                    str(initialized),
                ]
            )
            != 2
        ):
            raise AssertionError("config init replaced a file without --force")
        if cli_main(
            [
                "config",
                "init",
                str(zero_fixture),
                "--no-config",
                "--force",
                "--output",
                str(initialized),
            ]
        ):
            raise AssertionError("config init --force failed")
        query_report = Path(directory) / "query.md"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--path",
                "py/pkg",
                "--output",
                str(query_report),
            ]
        )
        if status or not query_report.read_text(encoding="utf-8").startswith(
            "# Focused architecture map: map-base\n"
        ):
            raise AssertionError("native Python default query Markdown CLI failed")
        context_query_report = Path(directory) / "query-context.md"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--path",
                "py/pkg",
                "--context-profile",
                "exact",
                "--candidate",
                "collapse",
                "--context-quota",
                "files=1",
                "--output",
                str(context_query_report),
            ]
        )
        context_text = context_query_report.read_text(encoding="utf-8")
        if (
            status
            or "Context: profile=exact;" not in context_text
            or "candidate=collapse;" not in context_text
            or "files=1/2." not in context_text
            or "## Selection manifest" not in context_text
        ):
            raise AssertionError("native Python context query CLI failed")
        current_map = Path(directory) / "current-map.json"
        current_map.write_bytes(duplicate_call_json)
        diff_output = Path(directory) / "diff.json"
        status = cli_main(
            [
                "diff",
                "--before",
                str(saved_map),
                "--after",
                str(current_map),
                "--check",
                "calls",
                "--output",
                str(diff_output),
            ]
        )
        if status != 1 or json.loads(diff_output.read_bytes())["artifact"] != "diff":
            raise AssertionError("native Python diff CLI lost call-risk status")
        workspace_output = Path(directory) / "workspace.json"
        status = cli_main(
            [
                "workspace",
                "--config",
                str(repository / "test/fixtures/workspace.json"),
                "--check",
                "--output",
                str(workspace_output),
            ]
        )
        if (
            status
            or json.loads(workspace_output.read_bytes())["artifact"] != "workspace"
        ):
            raise AssertionError("native Python workspace CLI failed")
        for graph_format in ("graphml", "mermaid"):
            graph_output = Path(directory) / f"map.{graph_format}"
            status = cli_main(
                [
                    "export",
                    graph_format,
                    "--map",
                    str(saved_map),
                    "--output",
                    str(graph_output),
                ]
            )
            if status or not graph_output.read_bytes().strip():
                raise AssertionError(f"native Python {graph_format} export CLI failed")
        okf_output = Path(directory) / "okf"
        status = cli_main(
            [
                "export",
                "okf",
                "--map",
                str(saved_map),
                "--output",
                str(okf_output),
            ]
        )
        if status or not (okf_output / "provenance" / "integrity.md").is_file():
            raise AssertionError("native Python OKF export CLI failed")
        status = cli_main(
            [
                "export",
                "okf",
                "--map",
                str(saved_map),
                "--output",
                str(okf_output),
                "--replace",
            ]
        )
        if status:
            raise AssertionError("native Python OKF replacement CLI failed")
        output = Path(directory) / "verification.json"
        status = cli_main(
            [
                "verify",
                "PYTHON-ENTRY",
                "--config",
                str(fixture / "archbird.json"),
                "--root",
                str(fixture),
                "--check",
                "--output",
                str(output),
            ]
        )
        output_document = json.loads(output.read_bytes())
        if (
            status != 1
            or output_document["artifact"] != "verification"
            or output_document["policy"]["requested_ids"] != ["PYTHON-ENTRY"]
            or output_document["constraints"][0]["status"] != "fail"
        ):
            raise AssertionError("native Python named constraint CLI failed")
        baseline_output = Path(directory) / "constraints.baseline.json"
        freeze_report = Path(directory) / "constraints.freeze-result.json"
        status = cli_main(
            [
                "verify",
                "--config",
                str(fixture / "archbird.json"),
                "--root",
                str(fixture),
                "--freeze",
                str(baseline_output),
                "--freeze-owner",
                "architecture",
                "--freeze-rationale",
                "Review current fixture constraint debt.",
                "--no-cache",
                "--output",
                str(freeze_report),
            ]
        )
        if (
            status
            or json.loads(baseline_output.read_bytes())["artifact"]
            != "constraint-baseline"
        ):
            raise AssertionError("native Python verify --freeze failed")
        for report_format in ("markdown", "sarif", "junit"):
            report_output = Path(directory) / f"verification.{report_format}"
            status = cli_main(
                [
                    "verify",
                    "PYTHON-ENTRY",
                    "--config",
                    str(fixture / "archbird.json"),
                    "--root",
                    str(fixture),
                    "--format",
                    report_format,
                    "--output",
                    str(report_output),
                ]
            )
            if status or not report_output.read_bytes().strip():
                raise AssertionError(
                    f"native Python {report_format} verification CLI failed"
                )
    print("native Python repository host passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
