"""Build the CPython host with a self-contained libarchbird snapshot."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


ROOT = Path(__file__).resolve().parent
SCHEMAS = ROOT / "archbird/schemas"


def _validate_schema_snapshot() -> None:
    manifest_path = SCHEMAS / ".archbird-manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("the content-hashed JSON schema snapshot is missing") from error
    if (
        manifest.get("artifact") != "archbird-python-schemas"
        or manifest.get("schema_version") != 1
        or not isinstance(manifest.get("files"), list)
    ):
        raise RuntimeError("invalid JSON schema snapshot manifest")
    paths: list[str] = []
    for row in manifest["files"]:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            raise RuntimeError("invalid JSON schema snapshot entry")
        relative = PurePosixPath(row["path"])
        candidate = SCHEMAS.joinpath(*relative.parts)
        if (
            relative.is_absolute()
            or ".." in relative.parts
            or not candidate.is_file()
            or candidate.is_symlink()
            or candidate.stat().st_size != row.get("bytes")
            or hashlib.sha256(candidate.read_bytes()).hexdigest()
            != row.get("sha256")
        ):
            raise RuntimeError(
                f"JSON schema snapshot does not match manifest: {relative}"
            )
        paths.append(relative.as_posix())
    if paths != sorted(set(paths)) or "archbird.schema.json" not in paths:
        raise RuntimeError("JSON schema snapshot paths must be sorted and unique")
    actual = {
        path.name
        for path in SCHEMAS.glob("*.json")
        if path.is_file() and path.name != ".archbird-manifest.json"
    }
    if actual != set(paths):
        raise RuntimeError("JSON schema snapshot inventory differs from manifest")


_validate_schema_snapshot()

# Editable source development deliberately uses the generic root shared C ABI
# through ``archbird/_native.py``.  Building a CPython extension in editable
# mode would shadow that adapter and silently retain stale C objects after a
# source edit.  Release wheels/sdists never set this explicit opt-in.
editable_shared = os.environ.get("ARCHBIRD_EDITABLE_SHARED", "0")
if editable_shared not in {"0", "1"}:
    raise RuntimeError("ARCHBIRD_EDITABLE_SHARED must be 0 or 1")
if editable_shared == "1":
    if any(command in sys.argv for command in ("bdist_wheel", "sdist")):
        raise RuntimeError(
            "ARCHBIRD_EDITABLE_SHARED is only valid for editable installation"
        )
    setup()
    raise SystemExit(0)

CSRC = ROOT / "csrc"
TREE_SITTER_PACKS = (
    ("C", "c"),
    ("CPP", "cpp"),
    ("PYTHON", "python"),
    ("JAVASCRIPT", "javascript"),
    ("TYPESCRIPT", "typescript"),
    ("TSX", "tsx"),
    ("R", "r"),
)
manifest_path = CSRC / ".archbird-manifest.json"
try:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
except (OSError, ValueError) as error:
    raise RuntimeError("the content-hashed C source snapshot is missing") from error
if manifest.get("artifact") != "archbird-python-csrc" or manifest.get(
    "schema_version"
) != 1:
    raise RuntimeError("invalid C source snapshot manifest")
rows = manifest.get("files")
if not isinstance(rows, list) or not rows:
    raise RuntimeError("C source snapshot manifest has no files")
paths_list = []
source_by_path: dict[str, str] = {}
for row in rows:
    if not isinstance(row, dict) or not isinstance(row.get("path"), str):
        raise RuntimeError("invalid C source snapshot entry")
    relative = PurePosixPath(row["path"])
    source = PurePosixPath(row.get("source", row["path"]))
    if relative.is_absolute() or ".." in relative.parts:
        raise RuntimeError(f"unsafe C source snapshot path: {relative}")
    if source.is_absolute() or ".." in source.parts:
        raise RuntimeError(f"unsafe C source identity path: {source}")
    candidate = CSRC.joinpath(*relative.parts)
    if (
        not candidate.is_file()
        or candidate.is_symlink()
        or candidate.stat().st_size != row.get("bytes")
        or hashlib.sha256(candidate.read_bytes()).hexdigest() != row.get("sha256")
    ):
        raise RuntimeError(f"C source snapshot does not match manifest: {relative}")
    relative_text = relative.as_posix()
    paths_list.append(relative_text)
    source_by_path[relative_text] = source.as_posix()
if paths_list != sorted(set(paths_list)):
    raise RuntimeError("C source snapshot paths must be sorted and unique")
paths = tuple(paths_list)


def _canonical_paths(values: set[str] | tuple[str, ...]) -> tuple[str, ...]:
    by_source = {source_by_path[path]: path for path in values}
    return tuple(by_source[source] for source in sorted(by_source))


class ContentAddressedBuildExt(build_ext):
    """Invalidate native objects when the reproducible C snapshot changes.

    Snapshot mtimes are deliberately fixed so wheels and sdists are
    reproducible.  Timestamp-only build backends would otherwise reuse stale
    objects after ``tools/sync_csrc.py python`` updates the source bytes.
    """

    def run(self) -> None:
        fingerprint = hashlib.sha256()
        fingerprint.update(b"archbird-python-build-v1\0")
        fingerprint.update(manifest_path.read_bytes())
        fingerprint.update(b"\0")
        fingerprint.update(Path(__file__).read_bytes())
        fingerprint.update(b"\0")
        fingerprint.update((ROOT / "pyproject.toml").read_bytes())
        expected = fingerprint.hexdigest()
        stamp = Path(self.build_temp) / ".archbird-build-sha256"
        try:
            current = stamp.read_text(encoding="ascii").strip()
        except OSError:
            current = ""
        if current != expected:
            self.force = True
        super().run()
        stamp.parent.mkdir(parents=True, exist_ok=True)
        temporary = stamp.with_suffix(".tmp")
        temporary.write_text(expected + "\n", encoding="ascii")
        temporary.replace(stamp)


def _version() -> str:
    match = re.search(
        r'^version\s*=\s*"([^"]+)"',
        (ROOT / "pyproject.toml").read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not match:
        raise RuntimeError("pyproject.toml has no project version")
    return match.group(1)


def _source_digest(relative: str) -> str:
    return hashlib.sha256((CSRC / relative).read_bytes()).hexdigest()


def _require_sha256(name: str, value: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise RuntimeError(
            f"{name} must contain exactly 64 lowercase hexadecimal characters"
        )
    return value


lexical_common_identity = tuple(
    line.strip()
    for line in (CSRC / "src/evidence/lexical/provider_identity.sources")
    .read_text(encoding="ascii")
    .splitlines()
    if line.strip()
)
duplicate_identity_path = len(lexical_common_identity) != len(
    set(lexical_common_identity)
)
if duplicate_identity_path or any(path not in paths for path in lexical_common_identity):
    raise RuntimeError("invalid lexical provider identity source manifest")
lexical_common_material = "".join(
    _source_digest(path) for path in lexical_common_identity
)
python_syntax_material = "".join(
    _source_digest(path)
    for path in paths
    if path.startswith("src/evidence/python/")
    and path.endswith((".c", ".h"))
)
lexical_digests = {
    language: hashlib.sha256(
        (
            lexical_common_material
            + _source_digest(f"src/evidence/lexical/{directory}/scanner.c")
            + _source_digest(f"src/evidence/lexical/{directory}/scanner.h")
            + (python_syntax_material if language == "PYTHON" else "")
        ).encode("ascii")
    ).hexdigest()
    for language, directory in (
        ("C", "c"),
        ("JAVASCRIPT", "javascript"),
        ("PYTHON", "python"),
        ("R", "r"),
    )
}
for lexical_name, lexical_digest in lexical_digests.items():
    _require_sha256(
        f"lexical {lexical_name} implementation digest", lexical_digest
    )
implementation_paths = tuple(
    path
    for path in paths
    if path.startswith(("include/", "src/", "vendor/"))
    and path.endswith((".c", ".h"))
)
implementation_material = "".join(
    f"{source_by_path[path]}:{_source_digest(path)}\n"
    for path in _canonical_paths(implementation_paths)
)
implementation_digest = hashlib.sha256(
    implementation_material.encode("ascii")
).hexdigest()
_require_sha256("core implementation digest", implementation_digest)
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
tree_sitter_digests = {}
for name, directory in TREE_SITTER_PACKS:
    pack_paths = set(tree_sitter_common)
    pack_paths.update(
        path
        for path in paths
        if path.startswith(f"src/evidence/syntax/tree_sitter/{directory}/")
        or (
            path.startswith(f"vendor/tree-sitter-{directory}/")
            and path.endswith((".c", ".h"))
        )
    )
    if name in {"JAVASCRIPT", "TYPESCRIPT", "TSX"}:
        pack_paths.update(
            path
            for path in paths
            if path.startswith("src/evidence/syntax/tree_sitter/ecmascript/")
        )
    if name == "TSX":
        pack_paths.update(
            path
            for path in paths
            if path.startswith("src/evidence/syntax/tree_sitter/typescript/")
        )
    if name == "PYTHON":
        pack_paths.update(
            path
            for path in paths
            if path.startswith("src/evidence/python/")
            and path.endswith((".c", ".h"))
        )
    tree_sitter_material = "".join(
        f"{source_by_path[path]}:{_source_digest(path)}\n"
        for path in _canonical_paths(pack_paths)
    )
    tree_sitter_digests[name] = hashlib.sha256(
        tree_sitter_material.encode("ascii")
    ).hexdigest()
    _require_sha256(
        f"Tree-sitter {name} implementation digest",
        tree_sitter_digests[name],
    )
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
    f"{source_by_path[path]}:{_source_digest(path)}\n"
    for path in _canonical_paths(scip_paths)
)
scip_digest = hashlib.sha256(scip_material.encode("ascii")).hexdigest()
_require_sha256("SCIP implementation digest", scip_digest)
sources = [
    str(Path("csrc") / path)
    for path in paths
    if path.endswith(".c")
    and not (
        path.startswith("vendor/tree-sitter/lib/src/")
        and path != "vendor/tree-sitter/lib/src/lib.c"
    )
]
source_include_dirs = sorted(
    {
        str(Path("csrc") / PurePosixPath(path).parent)
        for path in paths
        if path.startswith("src/") and path.endswith((".c", ".h"))
    }
)
tree_sitter_include_dirs = sorted(
    {
        str(Path("csrc") / PurePosixPath(path).parent)
        for path in paths
        if path.startswith("vendor/tree-sitter-")
        and path.endswith((".c", ".h"))
    }
)

if os.name == "nt":
    compile_args = ["/std:c11", "/W4"]
    link_args: list[str] = []
else:
    compile_args = [
        "-std=c11",
        "-g0",
        "-fvisibility=hidden",
        "-Wno-cast-function-type",
        "-Wno-overlength-strings",
    ]
    link_args = [] if sys.platform == "darwin" else ["-Wl,--strip-debug"]


setup(
    cmdclass={"build_ext": ContentAddressedBuildExt},
    ext_modules=[
        Extension(
            "archbird._native",
            sources=sources,
            include_dirs=[
                "csrc/include",
                *source_include_dirs,
                "csrc/vendor/yyjson",
                "csrc/vendor/pcre2",
                "csrc/vendor/tree-sitter/lib/include",
                "csrc/vendor/tree-sitter/lib/src",
                "csrc/src/evidence/syntax/tree_sitter",
                *tree_sitter_include_dirs,
            ],
            define_macros=[
                *[
                    (
                        f"ARCHBIRD_LEXICAL_{language}_IMPLEMENTATION_SHA256",
                        f'"{digest}"',
                    )
                    for language, digest in lexical_digests.items()
                ],
                ("ARCHBIRD_IMPLEMENTATION_SHA256", f'"{implementation_digest}"'),
                ("ARCHBIRD_SCIP_IMPLEMENTATION_SHA256", f'"{scip_digest}"'),
                *[
                    (
                        f"ARCHBIRD_TREE_SITTER_{name}_IMPLEMENTATION_SHA256",
                        f'"{digest}"',
                    )
                    for name, digest in tree_sitter_digests.items()
                ],
                ("ARCHBIRD_VERSION", '"0.0.1"'),
                *[
                    (f"ARCHBIRD_HAVE_TREE_SITTER_{name}", "1")
                    for name, _directory in TREE_SITTER_PACKS
                ],
                ("TREE_SITTER_REUSE_ALLOCATOR", "1"),
                ("_DEFAULT_SOURCE", "1"),
                ("PCRE2_CODE_UNIT_WIDTH", "8"),
                ("HAVE_CONFIG_H", None),
                ("PCRE2_STATIC", None),
                ("SUPPORT_PCRE2_8", None),
                ("SUPPORT_UNICODE", None),
                ("TREE_SITTER_HIDE_SYMBOLS", "1"),
                ("TREE_SITTER_HIDDEN_SYMBOLS", "1"),
            ],
            # Debian/Ubuntu's CPython build flags include ``-g`` even for
            # third-party release extensions.  Disable and strip that debug
            # metadata so wheels do not retain ephemeral build-root paths.
            extra_compile_args=compile_args,
            extra_link_args=link_args,
        )
    ]
)
