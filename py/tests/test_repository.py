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
import subprocess
import sys
import tempfile
import time
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
        render_source_markdown,
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
    unified = _native.unified_diff(
        b"same\nold\n",
        b"same\nnew\n",
        "src/example.txt",
        "src/example.txt",
        metadata=b"",
        context_lines=3,
        max_work_bytes=16 * 1024 * 1024,
    )
    if unified != (
        b"diff --git a/src/example.txt b/src/example.txt\n"
        b"--- a/src/example.txt\n"
        b"+++ b/src/example.txt\n"
        b"@@ -1,2 +1,2 @@\n"
        b" same\n"
        b"-old\n"
        b"+new\n"
    ):
        raise AssertionError("native Python unified-diff binding diverged")
    json_source = (
        b'{\n  "exports": {\n    ".": "./old.js"\n  }\n}\n'
    )
    json_edit = _native.json_pointer_edit(
        json_source,
        hashlib.sha256(json_source).hexdigest(),
        "/exports/.",
        b'"./old.js"',
        b'"./dist/index.js"',
    )
    if (
        json_edit["kind"] != "replace"
        or json_edit["matched_values"] != 1
        or json_edit["replacement"] != b'"./dist/index.js"'
        or (
            json_source[: json_edit["start_byte"]]
            + json_edit["replacement"]
            + json_source[json_edit["end_byte"] :]
        )
        != b'{\n  "exports": {\n    ".": "./dist/index.js"\n  }\n}\n'
    ):
        raise AssertionError("native Python JSON Pointer edit binding diverged")

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
    streamed_map = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    streamed_output = io.BytesIO()
    streamed_map.write_map_json(streamed_output.write)
    if streamed_map.map_cache_stats["writes"] != 1:
        raise AssertionError(
            "streaming Python Map output did not populate the complete Map cache"
        )
    streamed_warm = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    if (
        streamed_warm.map_json() != streamed_output.getvalue()
        or streamed_warm.map_cache_stats["hits"] != 1
    ):
        raise AssertionError("streaming Python Map cache was not reusable")
    shutil.rmtree(map_cache_root, ignore_errors=True)
    pretty_streamed = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    pretty_output = io.BytesIO()
    pretty_streamed.write_map_json(pretty_output.write, pretty=True)
    if not pretty_output.getvalue().startswith(b"{\n"):
        raise AssertionError("pretty streaming Python Map output is not formatted")
    pretty_warm = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    if pretty_warm.map_json() != first:
        raise AssertionError("pretty streaming stored noncanonical Map cache bytes")
    shutil.rmtree(map_cache_root, ignore_errors=True)
    skipped_stream = Project.from_config(
        fixture / "archbird.json",
        root=fixture,
        cache_dir=map_cache_root,
        cache_max_bytes=1,
    )
    skipped_output = io.BytesIO()
    skipped_stream.write_map_json(skipped_output.write)
    if (
        not skipped_output.getvalue()
        or skipped_stream.map_cache_stats["skipped"] != 1
        or tuple((map_cache_root / "maps-v1").rglob("*.json"))
    ):
        raise AssertionError("bounded streaming Map cache changed output or stored data")
    shutil.rmtree(map_cache_root, ignore_errors=True)
    failed_stream = Project.from_config(
        fixture / "archbird.json", root=fixture, cache_dir=map_cache_root
    )
    try:
        failed_stream.write_map_json(lambda _chunk: 0)
    except OSError:
        pass
    else:
        raise AssertionError("cached streaming accepted a short output write")
    if tuple((map_cache_root / "maps-v1").rglob("*.json")):
        raise AssertionError("failed streaming output populated the Map cache")
    if tuple(map_cache_root.rglob("*.tmp")):
        raise AssertionError("failed streaming output retained a cache temporary")
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
    remaining = tuple((bounded_root / "providers-v1").rglob("*.json"))
    if len(remaining) != 1:
        raise AssertionError("bounded Python cache fixture is not singular")
    missing_target = remaining[0]
    missing_target.unlink()
    bounded.reject(**second_parameters)
    if bounded.stats.bytes != 0:
        raise AssertionError("missing rejected cache entry stayed accounted")
    map_parameters = {
        "namespace": "fixture",
        "project": "cache-budget",
        "manifest_sha256": "3" * 64,
        "config_sha256": "4" * 64,
    }
    bounded.store_map(b"{}", **map_parameters)
    map_targets = tuple((bounded_root / "maps-v1").rglob("*.json"))
    if len(map_targets) != 1:
        raise AssertionError("bounded Python Map cache fixture is not singular")
    map_targets[0].unlink()
    bounded.reject_map(**map_parameters)
    if bounded.stats.bytes != 0:
        raise AssertionError("missing rejected Map cache entry stayed accounted")
    temporary_parent = bounded_root / "providers-v1" / "aa"
    temporary_parent.mkdir(parents=True, exist_ok=True)
    active = temporary_parent / (
        provider_cache_module._temporary_prefix(temporary_parent / "active.json")
        + "active.tmp"
    )
    active.write_bytes(b"active")
    recovered = ProviderCache(bounded_root, max_bytes=100)
    if not active.exists() or recovered.stats.temporaries_removed:
        raise AssertionError("Python cache removed an active writer temporary")
    active.unlink()
    owner = subprocess.Popen([sys.executable, "-c", "pass"])
    owner.wait()
    abandoned = temporary_parent / (
        provider_cache_module._temporary_prefix(
            temporary_parent / "abandoned.json", pid=owner.pid
        )
        + "abandoned.tmp"
    )
    abandoned.write_bytes(b"abandoned")
    legacy = temporary_parent / ".legacy.tmp"
    legacy.write_bytes(b"unowned")
    foreign = temporary_parent / (
        provider_cache_module._temporary_prefix(
            temporary_parent / "foreign.json",
            domain="f" * 16,
            pid=owner.pid,
        )
        + "foreign.tmp"
    )
    foreign.write_bytes(b"foreign")
    reclaimed = ProviderCache(bounded_root, max_bytes=100)
    if (
        abandoned.exists()
        or not legacy.exists()
        or not foreign.exists()
        or reclaimed.stats.temporaries_removed != 1
    ):
        raise AssertionError(
            "Python cache temporary ownership reclamation is unsafe"
        )
    with (
        mock.patch.object(provider_cache_module.os, "name", "nt"),
        mock.patch.object(provider_cache_module.os, "kill") as unsafe_kill,
    ):
        if provider_cache_module._temporary_owner_is_dead(abandoned):
            raise AssertionError("non-POSIX cache owner was classified as dead")
        unsafe_kill.assert_not_called()
    oversized_owner = temporary_parent / (
        provider_cache_module._temporary_prefix(
            temporary_parent / "oversized.json",
            pid=int("9" * 100),
        )
        + "oversized.tmp"
    )
    if provider_cache_module._temporary_owner_is_dead(oversized_owner):
        raise AssertionError("oversized cache owner was classified as dead")
    legacy.unlink()
    foreign.unlink()
    writer = bounded.open_map_writer(**map_parameters)
    writer.write(b"{}")
    if writer._temporary is None:
        raise AssertionError("Python Map cache writer has no owned temporary")
    concurrent = ProviderCache(bounded_root, max_bytes=100)
    if (
        not writer._temporary.exists()
        or concurrent.stats.temporaries_removed
    ):
        raise AssertionError(
            "concurrent Python cache inventory removed an active Map write"
        )
    writer.commit()
    if bounded.load_map(**map_parameters) != b"{}":
        raise AssertionError("Python Map cache writer failed after inventory")
    cross_process_root = bounded_root / "cross-process"
    ready = bounded_root / "cross-process.ready"
    release = bounded_root / "cross-process.release"
    child_code = """
import sys
import time
from pathlib import Path
from archbird.provider_cache import ProviderCache

root, ready, release = map(Path, sys.argv[1:4])
parameters = {
    "namespace": "fixture",
    "project": "cache-budget",
    "manifest_sha256": "3" * 64,
    "config_sha256": "4" * 64,
}
writer = ProviderCache(root, max_bytes=100).open_map_writer(**parameters)
writer.write(b"{}")
if writer._temporary is None:
    raise RuntimeError("child writer did not create a temporary")
ready.write_text(str(writer._temporary), encoding="utf-8")
while not release.exists():
    time.sleep(0.01)
writer.commit()
"""
    child = subprocess.Popen(
        [
            sys.executable,
            "-c",
            child_code,
            str(cross_process_root),
            str(ready),
            str(release),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={
            **os.environ,
            "PYTHONPATH": str(repository / "py"),
        },
    )
    try:
        deadline = time.monotonic() + 5.0
        while not ready.exists() and child.poll() is None:
            if time.monotonic() >= deadline:
                raise AssertionError("cross-process cache writer did not start")
            time.sleep(0.01)
        if not ready.exists():
            _, stderr = child.communicate(timeout=1.0)
            raise AssertionError(
                f"cross-process cache writer failed: {stderr.decode()}"
            )
        child_temporary = Path(ready.read_text(encoding="utf-8"))
        observer = ProviderCache(cross_process_root, max_bytes=100)
        if (
            not child_temporary.exists()
            or observer.stats.temporaries_removed
        ):
            raise AssertionError(
                "concurrent process removed an active Map cache write"
            )
        release.touch()
        _, stderr = child.communicate(timeout=5.0)
        if child.returncode:
            raise AssertionError(
                f"cross-process cache writer failed: {stderr.decode()}"
            )
        if observer.load_map(**map_parameters) != b"{}":
            raise AssertionError(
                "cross-process Map cache publication was not reusable"
            )
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
    with mock.patch.object(
        provider_cache_module.tempfile,
        "NamedTemporaryFile",
        side_effect=OSError(errno.ENOSPC, "no space left on device"),
    ):
        recovered.store(b"d", **cache_parameters)
        recovered.store_map(b"{}", **map_parameters)
    if recovered.stats.no_space != 1 or recovered.stats.errors != 1:
        raise AssertionError("Python cache did not classify ENOSPC")
    if recovered.map_stats.no_space != 1 or recovered.map_stats.errors != 1:
        raise AssertionError("Python Map cache did not classify ENOSPC")
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
    for invalid_timeout in (0, -1, float("nan"), True):
        try:
            Project.from_config(
                fixture / "archbird.json",
                root=fixture,
                python_provider_timeout=invalid_timeout,
            )
        except ValueError:
            pass
        else:
            raise AssertionError(
                f"invalid Python provider timeout was accepted: {invalid_timeout!r}"
            )
    document = json.loads(first)
    if document["project"] != "map-base" or len(document["files"]) != 3:
        raise AssertionError("repository Map does not describe the fixture")

    source_project = Project(
        "source-views",
        (
            Source(
                "js/api.js",
                b"// ```` fence\n"
                b"export function outer(x) {\n"
                b"  class Inner { method() { return x; } }\n"
                b"  return new Inner();\n"
                b"}\n"
                b"export const other = 2;\n",
                language="javascript",
                layer="javascript",
            ),
            Source(
                "py/api.py",
                b"@staticmethod\ndef decorated():\n    return 3\n\n"
                b"def other():\n    return 4\n",
                language="python",
                layer="python",
            ),
            Source(
                "py/unicode.py",
                "def café():\r\n    return 'ready'\r\n\r\n"
                "def peer():\r\n    return 'peer'\r\n".encode(),
                language="python",
                layer="python",
            ),
            Source(
                "js/bindings.js",
                b"export const Bound = class Internal { run() { return 1; } };\n"
                b"const Local = class { stop() {} }, peer = 2;\n"
                b"let Assigned;\n"
                b"Assigned = class InternalAssignment { go() { return 3; } };\n",
                language="javascript",
                layer="javascript",
            ),
            Source(
                "r/api.R",
                b"run <- function(value) {\n  value + 1\n}\n"
                b"peer <- function(value) {\n  value - 1\n}\n",
                language="r",
                layer="r",
            ),
            Source(
                "ts/bindings.ts",
                b"export const Bound = class Internal {\n"
                b"  run(): number { return 1; }\n"
                b"};\nconst peer = 2;\n",
                language="typescript",
                layer="typescript",
            ),
            Source(
                "ts/view.tsx",
                b"export const View = () => <div>ready</div>;\n"
                b"const peer = 2;\n",
                language="typescript",
                layer="typescript",
            ),
            Source(
                "src/api.c",
                b"int first(void) { return 1; }\n"
                b"int second(void) { return 2; }\n",
                language="c",
                layer="c",
            ),
            Source(
                "src/same_line.c",
                b"int left(void) { return 1; } int right(void) { return 2; }\n",
                language="c",
                layer="c",
            ),
            Source(
                "src/template.cpp",
                b"template <typename T>\n"
                b"T identity(T value) { return value; }\n"
                b"int peer(void) { return 2; }\n",
                language="cpp",
                layer="cpp",
            ),
            Source(
                "src/overload.cpp",
                b"int overload(void) { return 1; } "
                b"int overload(int value) { return value; }\n",
                language="cpp",
                layer="cpp",
            ),
            Source(
                "assets/raw.bin",
                b"\xff\x00",
                language="text",
                layer="assets",
            ),
            Source(
                "assets/control.txt",
                b"safe\x1b[31munsafe\n",
                language="text",
                layer="assets",
            ),
            Source(
                "assets/empty.txt",
                b"",
                language="text",
                layer="assets",
            ),
        ),
    )
    source_project.set_config(
        json.dumps(
            {
                "project": "source-views",
                "components": [
                    {
                        "name": "native",
                        "paths": ["src/**"],
                    }
                ],
                "layers": [
                    {
                        "name": "javascript",
                        "role": "core",
                        "language": "javascript",
                        "globs": ["js/**/*.js"],
                    },
                    {
                        "name": "python",
                        "role": "core",
                        "language": "python",
                        "globs": ["py/**/*.py"],
                    },
                    {
                        "name": "r",
                        "role": "core",
                        "language": "r",
                        "globs": ["r/**/*.R"],
                    },
                    {
                        "name": "c",
                        "role": "core",
                        "language": "c",
                        "globs": ["src/**/*.c"],
                    },
                    {
                        "name": "cpp",
                        "role": "core",
                        "language": "cpp",
                        "globs": ["src/**/*.cpp"],
                    },
                    {
                        "name": "typescript",
                        "role": "core",
                        "language": "typescript",
                        "globs": ["ts/**/*.ts", "ts/**/*.tsx"],
                    },
                    {
                        "name": "assets",
                        "role": "support",
                        "language": "text",
                        "globs": ["assets/**"],
                    },
                ],
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
    )
    source_project.scan(map_cache=False)
    source_map = source_project.map_json()
    source_map_document = json.loads(source_map)
    by_path = {row["path"]: row for row in source_map_document["files"]}
    expected_extents = {
        ("js/api.js", "outer"),
        ("js/api.js", "outer.Inner"),
        ("js/bindings.js", "Assigned"),
        ("js/bindings.js", "Bound"),
        ("py/api.py", "decorated"),
        ("py/unicode.py", "café"),
        ("r/api.R", "run"),
        ("src/api.c", "first"),
        ("src/same_line.c", "left"),
        ("src/template.cpp", "identity"),
        ("ts/bindings.ts", "Bound"),
        ("ts/view.tsx", "View"),
    }
    actual_extents = {
        (path, symbol["name"])
        for path, row in by_path.items()
        for symbol in row["symbols"]
        if "extent" in symbol
    }
    if not expected_extents.issubset(actual_extents):
        raise AssertionError(
            f"portable providers omitted exact declaration extents: "
            f"{expected_extents - actual_extents!r}"
        )
    compact_source = render_source_markdown(
        source_project, source_map, compact=True
    ).decode()
    if "outer.Inner" not in compact_source or "return new Inner()" in compact_source:
        raise AssertionError("compact source view did not remain an outline")

    def source_query(symbol: str) -> bytes:
        return query_map_json(
            source_map, symbols=(symbol,), depth=0, test_depth=0
        )

    nested_source = source_project.source_markdown(
        artifact_json=source_query("js/api.js:outer.Inner"),
    ).decode()
    if (
        "class Inner { method() { return x; } }" not in nested_source
        or "export function outer" in nested_source
        or "export const other" in nested_source
    ):
        raise AssertionError("standard source view did not isolate the exact class")
    decorated_source = source_project.source_markdown(
        artifact_json=source_query("py/api.py:decorated"),
    ).decode()
    if (
        "@staticmethod\ndef decorated():" not in decorated_source
        or "def other" in decorated_source
    ):
        raise AssertionError("Python source extent omitted a decorator or leaked a peer")
    exported_source = source_project.source_markdown(
        artifact_json=source_query("js/api.js:outer"),
    ).decode()
    if (
        "export function outer" not in exported_source
        or "export const other" in exported_source
    ):
        raise AssertionError("ECMAScript source extent lost its export wrapper")
    bound_source = source_project.source_markdown(
        artifact_json=source_query("js/bindings.js:Bound"),
    ).decode()
    if (
        "export const Bound = class Internal" not in bound_source
        or "peer = 2" in bound_source
    ):
        raise AssertionError("bound ECMAScript class extent lost its declaration")
    local_source = source_project.source_markdown(
        artifact_json=source_query("js/bindings.js:Local"),
    ).decode()
    if "Local = class" not in local_source or "peer = 2" in local_source:
        raise AssertionError("multi-binding source extent leaked a sibling")
    assigned_source = source_project.source_markdown(
        artifact_json=source_query("js/bindings.js:Assigned"),
    ).decode()
    if (
        "Assigned = class InternalAssignment" not in assigned_source
        or "export const Bound" in assigned_source
    ):
        raise AssertionError("assignment-bound class extent lost its declaration")
    typed_source = source_project.source_markdown(
        artifact_json=source_query("ts/bindings.ts:Bound"),
    ).decode()
    if (
        "export const Bound = class Internal" not in typed_source
        or "const peer" in typed_source
    ):
        raise AssertionError("TypeScript binding extent lost its declaration")
    tsx_source = source_project.source_markdown(
        artifact_json=source_query("ts/view.tsx:View"),
    ).decode()
    if (
        "export const View = () => <div>ready</div>;" not in tsx_source
        or "const peer" in tsx_source
    ):
        raise AssertionError("TSX binding extent lost its declaration")
    template_source = source_project.source_markdown(
        artifact_json=source_query("src/template.cpp:identity"),
    ).decode()
    if (
        "template <typename T>" not in template_source
        or "int peer" in template_source
    ):
        raise AssertionError("C++ template extent lost its declaration wrapper")
    overload_source = source_project.source_markdown(
        artifact_json=source_query("src/overload.cpp:overload"),
    ).decode()
    if (
        overload_source.count("overload") < 2
        or "return 1" in overload_source
        or "return value" in overload_source
        or "could not be bound to one exact source extent" not in overload_source
    ):
        raise AssertionError(
            "ambiguous same-line overloads were not kept as an outline"
        )
    unicode_source = source_project.source_markdown(
        artifact_json=source_query("py/unicode.py:café"),
    ).decode()
    if (
        "def café():\r\n    return 'ready'" not in unicode_source
        or "def peer" in unicode_source
    ):
        raise AssertionError("Unicode/CRLF source extent was not byte-exact")
    r_source = source_project.source_markdown(
        artifact_json=source_query("r/api.R:run"),
    ).decode()
    if "run <- function(value)" not in r_source or "peer <-" in r_source:
        raise AssertionError("R source extent leaked a peer declaration")
    same_line_source = source_project.source_markdown(
        artifact_json=source_query("src/same_line.c:left"),
    ).decode()
    if (
        "int left(void) { return 1; }" not in same_line_source
        or "int right" in same_line_source
    ):
        raise AssertionError("same-line source extent leaked a peer declaration")
    multiple_selection = json.loads(
        query_map_json(
            source_map,
            symbols=("py/unicode.py:café", "src/same_line.c:left"),
            depth=0,
            test_depth=0,
        )
    )
    multiple_selection["files"].reverse()
    multiple_source = source_project.source_markdown(
        artifact_json=json.dumps(multiple_selection).encode(),
    ).decode()
    if (
        "def café()" not in multiple_source
        or "int left(void)" not in multiple_source
        or "def peer" in multiple_source
        or "int right" in multiple_source
    ):
        raise AssertionError("multi-file source selection changed exact ranges")
    complete_source = source_project.source_markdown(
        artifact_json=source_query("js/api.js:outer.Inner"),
        full=True,
    ).decode()
    if (
        "export const other = 2;" not in complete_source
        or "\n`````javascript\n" not in complete_source
        or "\n`````\n" not in complete_source
    ):
        raise AssertionError("full source view was not complete or fence-safe")
    path_source = source_project.source_markdown(
        artifact_json=query_map_json(
            source_map, paths=("src/api.c",), depth=0, test_depth=0
        ),
    ).decode()
    if "int first" not in path_source or "int second" not in path_source:
        raise AssertionError("exact path source view did not return the complete file")
    union_source = source_project.source_markdown(
        artifact_json=query_map_json(
            source_map,
            paths=("src/api.c",),
            symbols=("src/api.c:first",),
            depth=0,
            test_depth=0,
        ),
    ).decode()
    if "int first" not in union_source or "int second" not in union_source:
        raise AssertionError("path seed was weakened by a symbol seed in the same file")
    bounded_source = source_project.source_markdown(
        artifact_json=source_query("src/api.c:*"),
        max_chars=725,
    ).decode()
    if (
        len(bounded_source) > 725
        or bounded_source.count("int first") + bounded_source.count("int second")
        != 1
        or "Declaration records: 1" not in bounded_source
    ):
        raise AssertionError(
            "bounded source view did not preserve one complete declaration "
            f"record: {bounded_source!r}"
        )
    component_source = source_project.source_markdown(
        artifact_json=query_map_json(
            source_map, components=("native",), depth=0, test_depth=0
        ),
    ).decode()
    if (
        "## src/api.c" not in component_source
        or "int first(void) { return 1; }" in component_source
    ):
        raise AssertionError("component seed was incorrectly expanded as a file seed")
    binary_source = source_project.source_markdown(
        artifact_json=query_map_json(
            source_map, paths=("assets/raw.bin",), depth=0, test_depth=0
        ),
        full=True,
    ).decode()
    if (
        "Source is not safe UTF-8 text" not in binary_source
        or "\ufffd" in binary_source
    ):
        raise AssertionError("non-UTF-8 source was silently decoded")
    control_source = source_project.source_markdown(
        artifact_json=query_map_json(
            source_map, paths=("assets/control.txt",), depth=0, test_depth=0
        ),
        full=True,
    ).decode()
    if (
        "Source is not safe UTF-8 text" not in control_source
        or "\x1b" in control_source
    ):
        raise AssertionError("terminal control bytes leaked into source Markdown")
    empty_source = source_project.source_markdown(
        artifact_json=query_map_json(
            source_map, paths=("assets/empty.txt",), depth=0, test_depth=0
        ),
        full=True,
    ).decode()
    if "## assets/empty.txt" not in empty_source or "\n```text\n\n```\n" not in empty_source:
        raise AssertionError("empty source file was not represented exactly")
    bounded_source = source_project.source_markdown(
        artifact_json=source_map, compact=True, max_chars=800
    )
    if len(bounded_source) > 800 or b"Omitted source" not in bounded_source:
        raise AssertionError("bounded source output violated its complete-block ledger")
    try:
        source_project.source_markdown(
            artifact_json=source_map, compact=True, max_chars=1
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("source view accepted a budget smaller than metadata")
    malformed_map = copy.deepcopy(source_map_document)
    malformed_symbol = next(
        row["symbols"][0] for row in malformed_map["files"] if row["symbols"]
    )
    malformed_symbol["extent"] = {
        "start": 5,
        "end": 2,
    }
    try:
        source_project.source_markdown(
            artifact_json=json.dumps(malformed_map).encode(), compact=True
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("source view accepted a malformed declaration extent")
    valid_selection = json.loads(source_query("js/api.js:outer.Inner"))
    invalid_selections = []
    invalid_identity = copy.deepcopy(valid_selection)
    invalid_identity["evidence"]["input_sha256"] = "broken"
    invalid_selections.append(("invalid Map identity", invalid_identity))
    duplicate_match = copy.deepcopy(valid_selection)
    duplicate_match["matched_symbols"].append(
        copy.deepcopy(duplicate_match["matched_symbols"][0])
    )
    if source_project.source_markdown(
        artifact_json=json.dumps(duplicate_match).encode(),
    ) != source_project.source_markdown(
        artifact_json=json.dumps(valid_selection).encode(),
    ):
        raise AssertionError("duplicate Query matches changed source rendering")
    empty_scope = copy.deepcopy(valid_selection)
    empty_scope["matched_symbols"][0]["scope"] = ""
    for symbol in empty_scope["files"][0]["symbols"]:
        if (
            symbol["name"] == empty_scope["matched_symbols"][0]["name"]
            and symbol["kind"] == empty_scope["matched_symbols"][0]["kind"]
            and symbol["line"] == empty_scope["matched_symbols"][0]["line"]
        ):
            symbol["scope"] = ""
    if "class Inner" not in source_project.source_markdown(
        artifact_json=json.dumps(empty_scope).encode(),
    ).decode():
        raise AssertionError("source view rejected an allowed empty symbol scope")
    duplicate_file = copy.deepcopy(valid_selection)
    duplicate_file["files"].append(copy.deepcopy(duplicate_file["files"][0]))
    invalid_selections.append(("duplicate selected file", duplicate_file))
    absent_match = copy.deepcopy(valid_selection)
    absent_match["matched_symbols"][0]["name"] = "outer.Absent"
    invalid_selections.append(("absent matched symbol", absent_match))
    absent_scope = copy.deepcopy(valid_selection)
    absent_scope["matched_symbols"][0]["scope"] = "unrelated"
    invalid_selections.append(("mismatched symbol scope", absent_scope))
    wrong_file = copy.deepcopy(valid_selection)
    wrong_file["files"][0]["sha256"] = "0" * 64
    invalid_selections.append(("wrong selected file", wrong_file))
    for label, invalid_selection in invalid_selections:
        try:
            source_project.source_markdown(
                artifact_json=json.dumps(invalid_selection).encode(),
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"source view accepted {label}")
    unavailable_extent = copy.deepcopy(valid_selection)
    unavailable_extent["files"][0]["symbols"][0].pop("extent")
    unavailable_source = source_project.source_markdown(
        artifact_json=json.dumps(unavailable_extent).encode(),
    ).decode()
    if (
        "outer.Inner" not in unavailable_source
        or "return x" in unavailable_source
    ):
        raise AssertionError(
            "source view guessed a declaration boundary absent from evidence"
        )
    changed_project = Project(
        "source-views",
        (
            Source(
                source.path,
                source.data + (b"changed" if source.path == "src/api.c" else b""),
                language=source.language,
                layer=source.layer,
                roles=source.roles,
            )
            for source in source_project.sources
        ),
    )
    try:
        changed_project.source_markdown(
            artifact_json=query_map_json(
                source_map, paths=("src/api.c",), depth=0, test_depth=0
            ),
        )
    except RuntimeError:
        pass
    else:
        raise AssertionError("source view accepted bytes that differ from the Map")

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
    materialized_config = json.dumps(
        zero_resolution["effective_config"],
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    materialized_resolution = json.loads(
        resolve_discovery(zero_fixture, config=materialized_config)
    )
    zero_resolution_evidence = copy.deepcopy(zero_resolution)
    materialized_resolution_evidence = copy.deepcopy(materialized_resolution)
    for document in (zero_resolution_evidence, materialized_resolution_evidence):
        document.pop("origins")
        document.pop("sha256")
    if zero_resolution_evidence != materialized_resolution_evidence:
        raise AssertionError(
            "materialized configuration changed derived resolution evidence"
        )
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
    zero_project = Project.from_repository(zero_fixture, map_cache=False)
    zero_map = json.loads(zero_project.map_json())
    original_python = (zero_fixture / "src/main.py").read_bytes()
    original_javascript = (zero_fixture / "src/main.js").read_bytes()
    overlay_project = zero_project.with_source_overlay(
        {
            "src/main.py": b"def overlaid():\n    return 2\n",
            "src/main.js": None,
            "src/added.py": b"def added():\n    return 3\n",
        },
        map_cache=False,
    )
    overlay_map = json.loads(overlay_project.map_json())
    overlay_files = {row["path"]: row for row in overlay_map["files"]}
    if (
        "src/main.js" in overlay_files
        or "src/added.py" not in overlay_files
        or [row["name"] for row in overlay_files["src/main.py"]["symbols"]]
        != ["overlaid"]
        or [row["name"] for row in overlay_files["src/added.py"]["symbols"]]
        != ["added"]
        or (zero_fixture / "src/main.py").read_bytes() != original_python
        or (zero_fixture / "src/main.js").read_bytes() != original_javascript
        or (zero_fixture / "src/added.py").exists()
    ):
        raise AssertionError(
            "source overlay did not produce an isolated classified after-Map"
        )
    ignore_overlay = zero_project.with_source_overlay(
        {".gitignore": b"*.skip.py\nsrc/main.py\n"},
        map_cache=False,
    )
    ignore_overlay_files = {
        row["path"] for row in json.loads(ignore_overlay.map_json())["files"]
    }
    if (
        "ignored/drop.py" not in ignore_overlay_files
        or "src/main.py" in ignore_overlay_files
        or (zero_fixture / ".gitignore").read_bytes()
        != b"ignored/\n*.skip.py\n"
    ):
        raise AssertionError(
            "source overlay did not apply changed ignore rules exhaustively"
        )
    overlaid_config = (
        b'{"exclude":["src/main.py"],"project":"overlay-config"}'
    )
    config_overlay = zero_project.with_source_overlay(
        {"archbird.json": overlaid_config},
        config=overlaid_config,
        map_cache=False,
    )
    config_overlay_map = json.loads(config_overlay.map_json())
    if (
        config_overlay_map["project"] != "overlay-config"
        or any(
            row["path"] == "src/main.py"
            for row in config_overlay_map["files"]
        )
        or (zero_fixture / "archbird.json").read_bytes()
        == overlaid_config
    ):
        raise AssertionError(
            "source overlay did not resolve the isolated project configuration"
        )
    materialized_project = Project.from_repository(
        zero_fixture, config=materialized_config, map_cache=False
    )
    materialized_map = json.loads(materialized_project.map_json())
    if (
        zero_project.config_sha256 != materialized_project.config_sha256
        or zero_project.map_input_sha256 != materialized_project.map_input_sha256
    ):
        raise AssertionError("materialized configuration changed Map identity")
    zero_map_evidence = copy.deepcopy(zero_map)
    materialized_map_evidence = copy.deepcopy(materialized_map)
    zero_map_evidence.pop("discovery")
    materialized_map_evidence.pop("discovery")
    if zero_map_evidence != materialized_map_evidence:
        raise AssertionError(
            "equal Map identities describe different materialized evidence"
        )
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
    zero_report = zero_project.map_markdown(view="evidence").decode("utf-8")
    if (
        "unsupported-known=1" not in zero_report
        or "## Repository coverage" not in zero_report
        or "Coverage frontier: **unknown**" not in zero_report
        or "Evidence: graph=complete; exhaustive=yes" not in zero_report
    ):
        raise AssertionError(
            "Map Markdown conflated graph completeness with repository coverage"
        )
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
    if not standard_map.startswith(b"# map-base architecture evidence\n"):
        raise AssertionError("native Python standard Map report is invalid")
    if (
        b"## Architecture groups" not in standard_map
        or b"## File landmarks" not in standard_map
        or b"## Presentation accounting" not in standard_map
        or b"## Entities" in standard_map
    ):
        raise AssertionError(
            "native Python overview is not architecture-first"
        )
    language_map = project.map_markdown(
        view="architecture", group_by="language"
    )
    if (
        b"group `language`" not in language_map
        or b"## Architecture groups" not in language_map
    ):
        raise AssertionError("native Python language grouping is unavailable")
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
    project_verification_json = project.verify_json()
    symbol_graph_json = project.graph_view_json(
        view="symbols",
        query={"symbols": ["js/index.js:add"], "depth": 1, "test_depth": 1},
    )
    if not graphml.startswith(b"<?xml") or b"<node " not in graphml:
        raise AssertionError("native Python GraphML export is invalid")
    if not mermaid.startswith(b"%% Archbird components graph"):
        raise AssertionError("native Python Mermaid export is invalid")
    graph_document = json.loads(graph_json)
    project_verification_document = json.loads(project_verification_json)
    symbol_graph_document = json.loads(symbol_graph_json)
    if (
        graph_document["artifact"] != "archbird-graph-view"
        or graph_document["request"]["view"] != "components"
        or graph_document["source"]["artifact"] != "map"
    ):
        raise AssertionError("native Python component graph JSON is invalid")
    if project_verification_document["artifact"] != "verification":
        raise AssertionError("native Python Project Verify artifact is invalid")
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
        retrieval["contract"] != "archbird-lexical-ranking-v3"
        or retrieval["confidence"] != "candidate"
        or len(retrieval["hits"]) != 4
        or retrieval["hits"][0]["path"] != "py/pkg/api.py"
        or retrieval["hits"][0]["name"] != "twice"
        or retrieval_query["files"][0]["path"] != "py/pkg/api.py"
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
    coverage_map = json.loads(report_map_json)
    coverage_file = next(
        row for row in coverage_map["files"] if row["path"] == "js/main.js"
    )
    coverage_file["symbols"].append(
        {
            "kind": "function",
            "line": 99,
            "name": "bridge",
            "scope": "function",
            "signature": "bridge(quasar, handle)",
        }
    )
    coverage_map["components"].append(
        {
            "description": "",
            "files": ["js/main.js"],
            "name": "quasar-runtime",
            "outgoing": {},
            "symbol_count": 1,
        }
    )
    coverage_retrieval = json.loads(
        query_map_json(
            json.dumps(
                coverage_map, sort_keys=True, separators=(",", ":")
            ).encode(),
            search=["quasar handled"],
            search_limit=5,
            depth=0,
            test_depth=0,
        )
    )["query"]["retrieval"]
    if (
        coverage_retrieval["hits"][0]["kind"] != "component"
        or coverage_retrieval["hits"][0]["name"] != "quasar-runtime"
    ):
        raise AssertionError(
            "weak fuzzy matches received a full query-coverage boost: "
            f"{coverage_retrieval!r}"
        )
    weak_hit = next(
        hit for hit in coverage_retrieval["hits"] if hit.get("name") == "bridge"
    )
    if not any(
        reason["term"] == "handled"
        and reason["match"] == "edit-1"
        and reason["field"] == "symbol.signature"
        for reason in weak_hit["reasons"]
    ):
        raise AssertionError(
            f"soft coverage discarded its fuzzy witness: {weak_hit!r}"
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
        source_output = Path(directory) / "source.md"
        status = cli_main(
            [
                "query",
                "--map",
                str(saved_map),
                "--root",
                str(fixture),
                "--symbol",
                "js/index.js:add",
                "--depth",
                "0",
                "--test-depth",
                "0",
                "--view",
                "source",
                "--output",
                str(source_output),
            ]
        )
        source_text = source_output.read_text(encoding="utf-8")
        if (
            status
            or "export function add" not in source_text
            or "export const twice" in source_text
        ):
            raise AssertionError("saved-Map Python source view CLI failed")
        live_source_path = fixture / "js/index.js"
        live_source_bytes = live_source_path.read_bytes()
        live_source_path.write_bytes(
            live_source_bytes + b"export const changed = true;\n"
        )
        try:
            if (
                cli_main(
                    [
                        "query",
                        "--map",
                        str(saved_map),
                        "--root",
                        str(fixture),
                        "--symbol",
                        "js/index.js:add",
                        "--depth",
                        "0",
                        "--test-depth",
                        "0",
                        "--view",
                        "source",
                    ]
                )
                != 2
            ):
                raise AssertionError("saved source view accepted changed bytes")
        finally:
            live_source_path.write_bytes(live_source_bytes)
        live_source_backup = live_source_path.with_suffix(".js.original")
        live_source_path.rename(live_source_backup)
        live_source_path.symlink_to(live_source_backup.name)
        try:
            if (
                cli_main(
                    [
                        "query",
                        "--map",
                        str(saved_map),
                        "--root",
                        str(fixture),
                        "--symbol",
                        "js/index.js:add",
                        "--depth",
                        "0",
                        "--test-depth",
                        "0",
                        "--view",
                        "source",
                    ]
                )
                != 2
            ):
                raise AssertionError("saved source view accepted a symlink")
        finally:
            live_source_path.unlink()
            live_source_backup.rename(live_source_path)
        dump_output = Path(directory) / "dump.md"
        status = cli_main(
            [
                "query",
                str(fixture),
                "--symbol",
                "js/index.js:add",
                "--depth",
                "0",
                "--test-depth",
                "0",
                "--dump",
                "--no-cache",
                "--output",
                str(dump_output),
            ]
        )
        if status or "export const twice" not in dump_output.read_text(
            encoding="utf-8"
        ):
            raise AssertionError("Python --dump did not emit the selected file")
        if (
            cli_main(
                [
                    "query",
                    str(fixture),
                    "--symbol",
                    "add",
                    "--dump",
                    "--max-chars",
                    "100",
                    "--no-cache",
                    "--progress",
                    "never",
                ]
            )
            != 2
            or cli_main(
                [
                    "map",
                    str(fixture),
                    "--view",
                    "source",
                    "--group-by",
                    "directory",
                    "--no-cache",
                    "--progress",
                    "never",
                ]
            )
            != 2
        ):
            raise AssertionError("Python source-view option conflicts were accepted")
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
            "# map-base architecture evidence\n"
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
        if (
            status
            or initialized_document["project"] != "zero-fixture"
            or initialized_document != zero_resolution["effective_config"]
        ):
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
                "--format",
                "json",
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
                "--format",
                "json",
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
