#!/usr/bin/env python3
"""Build deterministic, self-contained C snapshots for package distributions."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
FIXED_MTIME = 946684800
MANIFEST = ".archbird-manifest.json"
TREE_SITTER_PACKS = (
    ("C", "c"),
    ("CPP", "cpp"),
    ("PYTHON", "python"),
    ("JAVASCRIPT", "javascript"),
    ("TYPESCRIPT", "typescript"),
    ("TSX", "tsx"),
    ("R", "r"),
)
PCRE2_SNAPSHOT_NAMES = (
    "config.h",
    "pcre2.h",
    "pcre2_auto_possess.c",
    "pcre2_chartables.c",
    "pcre2_chkdint.c",
    "pcre2_compile.c",
    "pcre2_compile.h",
    "pcre2_compile_cgroup.c",
    "pcre2_compile_class.c",
    "pcre2_config.c",
    "pcre2_context.c",
    "pcre2_convert.c",
    "pcre2_dfa_match.c",
    "pcre2_error.c",
    "pcre2_extuni.c",
    "pcre2_find_bracket.c",
    "pcre2_internal.h",
    "pcre2_intmodedep.h",
    "pcre2_jit_char_inc.h",
    "pcre2_jit_compile.c",
    "pcre2_jit_match_inc.h",
    "pcre2_jit_misc_inc.h",
    "pcre2_jit_simd_inc.h",
    "pcre2_maketables.c",
    "pcre2_match.c",
    "pcre2_match_data.c",
    "pcre2_match_next.c",
    "pcre2_newline.c",
    "pcre2_ord2utf.c",
    "pcre2_pattern_info.c",
    "pcre2_script_run.c",
    "pcre2_serialize.c",
    "pcre2_string_utils.c",
    "pcre2_study.c",
    "pcre2_substitute.c",
    "pcre2_substring.c",
    "pcre2_tables.c",
    "pcre2_ucd.c",
    "pcre2_ucp.h",
    "pcre2_ucptables_inc.h",
    "pcre2_util.h",
    "pcre2_valid_utf.c",
    "pcre2_xclass.c",
)


def _vendor_file_map() -> dict[str, str]:
    """Map package-snapshot paths to pinned upstream submodule paths."""

    result = {
        "vendor/yyjson/yyjson.c": "vendor/yyjson/src/yyjson.c",
        "vendor/yyjson/yyjson.h": "vendor/yyjson/src/yyjson.h",
        "vendor/yyjson/LICENSE": "vendor/yyjson/LICENSE",
        "vendor/pcre2/LICENSE": "vendor/pcre2/LICENCE.md",
    }
    for name in PCRE2_SNAPSHOT_NAMES:
        upstream = {
            "config.h": "config.h.generic",
            "pcre2.h": "pcre2.h.generic",
            "pcre2_chartables.c": "pcre2_chartables.c.dist",
        }.get(name, name)
        result[f"vendor/pcre2/{name}"] = f"vendor/pcre2/src/{upstream}"
    grammar_files = {
        "c": ("parser.c",),
        "cpp": ("parser.c", "scanner.c"),
        "javascript": ("parser.c", "scanner.c"),
        "python": ("parser.c", "scanner.c"),
        "r": ("parser.c", "scanner.c"),
    }
    generated_headers = (
        "tree_sitter/alloc.h",
        "tree_sitter/array.h",
        "tree_sitter/parser.h",
    )
    for directory, source_names in grammar_files.items():
        logical_root = f"vendor/tree-sitter-{directory}"
        upstream_root = f"vendor/tree-sitter-{directory}/src"
        result[f"{logical_root}/LICENSE"] = (
            f"vendor/tree-sitter-{directory}/LICENSE"
        )
        for name in (*source_names, *generated_headers):
            result[f"{logical_root}/{name}"] = f"{upstream_root}/{name}"
    for directory in ("typescript", "tsx"):
        logical_root = f"vendor/tree-sitter-{directory}"
        upstream_root = f"vendor/tree-sitter-typescript/{directory}"
        result[f"{logical_root}/LICENSE"] = "vendor/tree-sitter-typescript/LICENSE"
        result[f"{logical_root}/common/scanner.h"] = (
            "vendor/tree-sitter-typescript/common/scanner.h"
        )
        for name in ("parser.c", "scanner.c", *generated_headers):
            result[f"{logical_root}/{directory}/src/{name}"] = (
                f"{upstream_root}/src/{name}"
            )
    return result


VENDOR_FILE_MAP = _vendor_file_map()


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _source_identity(relative: str) -> str:
    return VENDOR_FILE_MAP.get(relative, relative)


def _source_path(relative: str) -> Path:
    return REPOSITORY / _source_identity(relative)


def _canonical_paths(paths: tuple[str, ...] | set[str]) -> tuple[str, ...]:
    by_identity = {_source_identity(path): path for path in paths}
    return tuple(by_identity[identity] for identity in sorted(by_identity))


def _repository_files(binding: str) -> tuple[str, ...]:
    patterns = (
        "include/**/*.h",
        "src/**/*.c",
        "src/**/*.h",
        "vendor/tree-sitter/lib/include/**/*.h",
        "vendor/tree-sitter/lib/src/**/*.c",
        "vendor/tree-sitter/lib/src/**/*.h",
        "vendor/tree-sitter/LICENSE",
    )
    paths = {binding, *VENDOR_FILE_MAP}
    for pattern in patterns:
        paths.update(
            path.relative_to(REPOSITORY).as_posix()
            for path in REPOSITORY.glob(pattern)
            if path.is_file()
        )
    missing = [path for path in paths if not _source_path(path).is_file()]
    if missing:
        raise RuntimeError(
            "missing pinned vendor/source inputs: " + ", ".join(sorted(missing))
        )
    return tuple(sorted(paths))


def _implementation_digests(
    paths: tuple[str, ...],
) -> tuple[str, dict[str, str], dict[str, str], str]:
    source_digest = {path: _digest(_source_path(path)) for path in paths}
    common = (
        "src/evidence/lexical/tokenizer.c",
        "src/evidence/lexical/tokenizer.h",
        "src/evidence/fact_builder.c",
        "src/evidence/fact_builder.h",
        "src/base/model.c",
        "src/base/model.h",
        "src/base/render.c",
        "src/base/render_internal.h",
        "src/base/sha256.c",
        "src/base/sha256.h",
    )
    common_material = "".join(source_digest[path] for path in common)
    lexical = {
        language: hashlib.sha256(
            (
                common_material
                + source_digest[f"src/evidence/lexical/{directory}/scanner.c"]
                + source_digest[f"src/evidence/lexical/{directory}/scanner.h"]
            ).encode("ascii")
        ).hexdigest()
        for language, directory in (
            ("C", "c"),
            ("JAVASCRIPT", "javascript"),
            ("PYTHON", "python"),
            ("R", "r"),
        )
    }
    tree_sitter_common = tuple(
        path
        for path in paths
        if path
        in {
            "src/evidence/syntax/tree_sitter/scanner.c",
            "src/evidence/syntax/tree_sitter/scanner.h",
            "src/evidence/syntax/tree_sitter/tree_sitter_allocator.c",
            "src/evidence/syntax/tree_sitter/tree_sitter_allocator.h",
            "src/evidence/fact_builder.c",
            "src/evidence/fact_builder.h",
            "src/base/model.c",
            "src/base/model.h",
        }
        or path.startswith("vendor/tree-sitter/lib/")
    )
    tree_sitter = {}
    for name, directory in TREE_SITTER_PACKS:
        pack_paths = set(tree_sitter_common)
        pack_paths.update(
            path
            for path in paths
            if path.startswith(
                f"src/evidence/syntax/tree_sitter/{directory}/"
            )
            or (
                path.startswith(f"vendor/tree-sitter-{directory}/")
                and path.endswith((".c", ".h"))
            )
        )
        if name in {"JAVASCRIPT", "TYPESCRIPT", "TSX"}:
            pack_paths.update(
                path
                for path in paths
                if path.startswith(
                    "src/evidence/syntax/tree_sitter/ecmascript/"
                )
            )
        if name == "TSX":
            pack_paths.update(
                path
                for path in paths
                if path.startswith(
                    "src/evidence/syntax/tree_sitter/typescript/"
                )
            )
        material = "".join(
            f"{_source_identity(path)}:{source_digest[path]}\n"
            for path in _canonical_paths(pack_paths)
        )
        tree_sitter[name] = hashlib.sha256(material.encode("ascii")).hexdigest()
    scip_paths = tuple(
        path
        for path in paths
        if path.startswith("src/evidence/semantic/scip/")
        or path
        in {
            "src/evidence/fact_builder.c",
            "src/evidence/fact_builder.h",
            "src/base/model.c",
            "src/base/model.h",
        }
    )
    scip_material = "".join(
        f"{_source_identity(path)}:{source_digest[path]}\n"
        for path in _canonical_paths(set(scip_paths))
    )
    scip = hashlib.sha256(scip_material.encode("ascii")).hexdigest()
    implementation_paths = tuple(
        path
        for path in paths
        if path.startswith(("include/", "src/", "vendor/"))
        and path.endswith((".c", ".h"))
    )
    material = "".join(
        f"{_source_identity(path)}:{source_digest[path]}\n"
        for path in _canonical_paths(set(implementation_paths))
    )
    return (
        hashlib.sha256(material.encode("ascii")).hexdigest(),
        lexical,
        tree_sitter,
        scip,
    )


def repository_implementation_digest() -> str:
    """Return the canonical shared-core digest used by both package builds."""

    digest, _lexical, _tree_sitter, _scip = _implementation_digests(
        _repository_files("bindings/python.c")
    )
    return digest


def _node_gypi(paths: tuple[str, ...]) -> bytes:
    implementation, lexical, tree_sitter, scip = _implementation_digests(paths)
    sources = [
        f"csrc/{path}"
        for path in paths
        if path.endswith(".c")
        and not (
            path.startswith("vendor/tree-sitter/lib/src/")
            and path != "vendor/tree-sitter/lib/src/lib.c"
        )
    ]
    include_dirs = sorted(
        {
            PurePosixPath(path).parent.as_posix()
            for path in paths
            if path.startswith("src/") and path.endswith((".c", ".h"))
        }
        | {
            "include",
            "vendor/yyjson",
            "vendor/pcre2",
            "vendor/tree-sitter/lib/include",
            "vendor/tree-sitter/lib/src",
            "src/evidence/syntax/tree_sitter",
        }
        | {
            PurePosixPath(path).parent.as_posix()
            for path in paths
            if path.startswith("vendor/tree-sitter-")
            and path.endswith((".c", ".h"))
        }
    )
    definitions = [
        f'ARCHBIRD_LEXICAL_{language}_IMPLEMENTATION_SHA256="{digest}"'
        for language, digest in sorted(lexical.items())
    ]
    definitions.extend(
        [
            f'ARCHBIRD_IMPLEMENTATION_SHA256="{implementation}"',
            f'ARCHBIRD_SCIP_IMPLEMENTATION_SHA256="{scip}"',
            'ARCHBIRD_VERSION="0.0.1"',
            "TREE_SITTER_REUSE_ALLOCATOR=1",
            "_DEFAULT_SOURCE=1",
            "HAVE_CONFIG_H",
            "NAPI_VERSION=8",
            "PCRE2_CODE_UNIT_WIDTH=8",
            "PCRE2_STATIC",
            "SUPPORT_PCRE2_8",
            "SUPPORT_UNICODE",
            "TREE_SITTER_HIDE_SYMBOLS=1",
            "TREE_SITTER_HIDDEN_SYMBOLS=1",
        ]
    )
    definitions.extend(
        f'ARCHBIRD_TREE_SITTER_{name}_IMPLEMENTATION_SHA256="{digest}"'
        for name, digest in sorted(tree_sitter.items())
    )
    definitions.extend(
        f"ARCHBIRD_HAVE_TREE_SITTER_{name}=1"
        for name, _directory in TREE_SITTER_PACKS
    )
    document = {
        "variables": {
            "archbird_defines": definitions,
            "archbird_include_dirs": include_dirs,
            "archbird_sources": sources,
        }
    }
    return json.dumps(document, indent=2, sort_keys=True).encode("utf-8") + b"\n"


def _manifest(
    artifact: str,
    paths: tuple[str, ...],
    generated: dict[str, bytes],
) -> dict:
    result = {
        "artifact": artifact,
        "schema_version": 1,
        "files": [
            {
                "bytes": _source_path(relative).stat().st_size,
                "path": relative,
                "sha256": _digest(_source_path(relative)),
                "source": _source_identity(relative),
            }
            for relative in paths
        ],
    }
    if generated:
        result["generated"] = [
            {
                "bytes": len(data),
                "path": relative,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            for relative, data in sorted(generated.items())
        ]
    return result


def _load(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _current(target: Path, expected: dict) -> bool:
    if _load(target / MANIFEST) != expected:
        return False
    for section in ("files", "generated"):
        for row in expected.get(section, []):
            candidate = target / row["path"]
            if (
                not candidate.is_file()
                or candidate.is_symlink()
                or candidate.stat().st_size != row["bytes"]
                or _digest(candidate) != row["sha256"]
            ):
                return False
    return True


def _copy_legal(package: Path) -> None:
    source = REPOSITORY / "LICENSE"
    destination = package / "LICENSE"
    if not destination.is_file() or destination.read_bytes() != source.read_bytes():
        destination.write_bytes(source.read_bytes())
    os.utime(destination, (FIXED_MTIME, FIXED_MTIME))


def sync(target_name: str) -> Path:
    targets = {
        "python": (REPOSITORY / "py", "bindings/python.c", "archbird-python-csrc"),
        "node": (REPOSITORY / "js", "bindings/node.c", "archbird-node-csrc"),
    }
    if target_name not in targets:
        raise ValueError(f"unknown package target: {target_name}")
    package, binding, artifact = targets[target_name]
    target = package / "csrc"
    paths = _repository_files(binding)
    generated = {"archbird.gypi": _node_gypi(paths)} if target_name == "node" else {}
    expected = _manifest(artifact, paths, generated)
    _copy_legal(package)
    if _current(target, expected):
        return target
    if target.exists() and _load(target / MANIFEST).get("artifact") != artifact:
        raise RuntimeError(f"refusing to replace unrecognized {target}")
    temporary = Path(tempfile.mkdtemp(prefix=".csrc-", dir=package))
    try:
        for relative in paths:
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(_source_path(relative).read_bytes())
            os.utime(destination, (FIXED_MTIME, FIXED_MTIME))
        for relative, data in generated.items():
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            os.utime(destination, (FIXED_MTIME, FIXED_MTIME))
        manifest_bytes = json.dumps(
            expected, ensure_ascii=False, indent=2, sort_keys=True
        ).encode("utf-8") + b"\n"
        (temporary / MANIFEST).write_bytes(manifest_bytes)
        os.utime(temporary / MANIFEST, (FIXED_MTIME, FIXED_MTIME))
        backup = package / ".csrc-previous"
        if backup.exists():
            shutil.rmtree(backup)
        if target.exists():
            os.replace(target, backup)
        os.replace(temporary, target)
        if backup.exists():
            shutil.rmtree(backup)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return target


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("target", choices=("python", "node"))
    args = parser.parse_args()
    print(sync(args.target))


if __name__ == "__main__":
    main()
