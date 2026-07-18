"""Decode OKF filesystem, YAML, and CommonMark syntax into normalized facts.

This module owns host I/O and ecosystem parser dependencies only.  It does not
decide whether an OKF concept satisfies an Archbird requirement and never turns
prose into a verification check.  Shared validation, indexing, link resolution,
and query policy belongs to the native interchange kernel.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import platform
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Mapping, Optional, Set, Tuple, Union
from urllib.parse import unquote, urlsplit

from ... import __version__, implementation_digest
from ...errors import ConfigError


MAX_DOCUMENT_BYTES = 4 * 1024 * 1024
MAX_DOCUMENTS = 100_000
MAX_EXPANDED_YAML_VALUES = 1_000_000
RESERVED_NAMES = {"index.md", "log.md"}


def _dependencies():
    try:
        import yaml  # type: ignore
        from markdown_it import MarkdownIt  # type: ignore
    except ImportError as error:
        raise ConfigError(
            "OKF input requires optional dependencies; install archbird[okf]"
        ) from error
    return yaml, MarkdownIt


def _yaml_loader(yaml):
    class Loader(yaml.SafeLoader):
        pass

    # OKF metadata must round-trip through JSON.  In particular, a timestamp is
    # preserved as the exact asserted string rather than becoming a host date.
    Loader.yaml_implicit_resolvers = {
        key: [row for row in values if row[0] != "tag:yaml.org,2002:timestamp"]
        for key, values in yaml.SafeLoader.yaml_implicit_resolvers.items()
    }

    def construct_mapping(loader, node, deep=False):
        explicit: Set[Any] = set()
        for key_node, _value_node in list(node.value):
            if key_node.tag == "tag:yaml.org,2002:merge":
                continue
            key = loader.construct_object(key_node, deep=False)
            try:
                duplicate = key in explicit
            except TypeError as error:
                raise ConfigError(
                    "OKF frontmatter: mapping keys must be scalar"
                ) from error
            if duplicate:
                raise ConfigError(f"OKF frontmatter: duplicate key {key!r}")
            explicit.add(key)
        loader.flatten_mapping(node)
        return yaml.SafeLoader.construct_mapping(loader, node, deep=deep)

    Loader.add_constructor(
        yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct_mapping
    )
    return Loader


def _validate_yaml_value(
    value: Any,
    where: str,
    *,
    active: Optional[Set[int]] = None,
    memo: Optional[Dict[int, Tuple[Any, int]]] = None,
    budget: Optional[List[int]] = None,
) -> Any:
    active = set() if active is None else active
    memo = {} if memo is None else memo
    budget = [0] if budget is None else budget

    def consume(amount: int) -> None:
        budget[0] += amount
        if budget[0] > MAX_EXPANDED_YAML_VALUES:
            raise ConfigError(
                f"{where}: expanded YAML exceeds {MAX_EXPANDED_YAML_VALUES} values"
            )

    if value is None or isinstance(value, (str, bool, int)):
        consume(1)
        return value
    if isinstance(value, float):
        consume(1)
        if not math.isfinite(value):
            raise ConfigError(f"{where}: expected finite number")
        return value
    if isinstance(value, (list, dict)):
        identity = id(value)
        if identity in active:
            raise ConfigError(f"{where}: recursive YAML aliases are not supported")
        if identity in memo:
            normalized, size = memo[identity]
            consume(size)
            return normalized
        before = budget[0]
        consume(1)
        active.add(identity)
        try:
            if isinstance(value, list):
                normalized: Any = [
                    _validate_yaml_value(
                        item,
                        f"{where}[{index}]",
                        active=active,
                        memo=memo,
                        budget=budget,
                    )
                    for index, item in enumerate(value)
                ]
            else:
                result: Dict[str, Any] = {}
                for key, item in value.items():
                    if not isinstance(key, str) or not key:
                        raise ConfigError(f"{where}: expected non-empty string keys")
                    result[key] = _validate_yaml_value(
                        item,
                        f"{where}.{key}",
                        active=active,
                        memo=memo,
                        budget=budget,
                    )
                normalized = result
            memo[identity] = (normalized, budget[0] - before)
            return normalized
        finally:
            active.remove(identity)
    raise ConfigError(f"{where}: unsupported YAML value {type(value).__name__}")


def _frontmatter(
    text: str, path: str, *, required: bool, allowed: bool
) -> Tuple[Mapping[str, Any], str]:
    yaml, _markdown = _dependencies()
    lines = text.splitlines(keepends=True)
    starts = bool(lines and lines[0].rstrip("\r\n") == "---")
    if not starts:
        if required:
            raise ConfigError(f"{path}: missing YAML frontmatter")
        return {}, text
    if not allowed:
        raise ConfigError(f"{path}: frontmatter is not permitted in this reserved file")
    boundary = next(
        (
            index
            for index, line in enumerate(lines[1:], 1)
            if line.rstrip("\r\n") == "---"
        ),
        None,
    )
    if boundary is None:
        raise ConfigError(f"{path}: unclosed YAML frontmatter")
    source = "".join(lines[1:boundary])
    try:
        loaded = yaml.load(source, Loader=_yaml_loader(yaml))
    except ConfigError:
        raise
    except yaml.YAMLError as error:
        raise ConfigError(f"{path}: invalid YAML frontmatter: {error}") from error
    loaded = {} if loaded is None else loaded
    loaded = _validate_yaml_value(loaded, f"{path}.frontmatter")
    if not isinstance(loaded, dict):
        raise ConfigError(f"{path}: frontmatter must be a mapping")
    return loaded, "".join(lines[boundary + 1 :])


def _diagnostic(severity: str, code: str, message: str, path: str = ""):
    return {"severity": severity, "code": code, "message": message, "path": path}


def _bundle_paths(root: Path):
    diagnostics = []
    result = []
    for current, directories, files in os.walk(root, followlinks=False):
        current_path = Path(current)
        for name in list(directories):
            path = current_path / name
            if path.is_symlink():
                diagnostics.append(
                    _diagnostic(
                        "warning",
                        "ignored-symlink-directory",
                        "symlink directories are not traversed",
                        path.relative_to(root).as_posix(),
                    )
                )
                directories.remove(name)
        for name in files:
            path = current_path / name
            if path.suffix.lower() != ".md":
                continue
            relative = path.relative_to(root).as_posix()
            if path.is_symlink():
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "symlink-document",
                        "OKF documents must be regular files",
                        relative,
                    )
                )
                continue
            result.append(path)
    result.sort(key=lambda path: path.relative_to(root).as_posix())
    if len(result) > MAX_DOCUMENTS:
        raise ConfigError(
            f"OKF bundle: {len(result)} documents exceeds limit {MAX_DOCUMENTS}"
        )
    return result, diagnostics


def _raw_links(body: str, path: str):
    _yaml, MarkdownIt = _dependencies()
    parser = MarkdownIt("commonmark")
    diagnostics = []
    rows: Set[Tuple[str, str]] = set()
    try:
        tokens = parser.parse(body)
    except Exception as error:
        diagnostics.append(_diagnostic("error", "invalid-markdown", str(error), path))
        return [], diagnostics
    for token in tokens:
        children = token.children or ()
        for index, child in enumerate(children):
            if child.type != "link_open":
                continue
            href = child.attrGet("href") or ""
            label_parts = []
            for nested in children[index + 1 :]:
                if nested.type == "link_close":
                    break
                if nested.type in {"text", "code_inline"}:
                    label_parts.append(nested.content)
            rows.add(("".join(label_parts), href))
    result = []
    for label, href in sorted(rows):
        parsed = urlsplit(href)
        result.append(
            {
                "label": label,
                "href": href,
                "repr": repr(href),
                "external": bool(parsed.scheme or parsed.netloc),
                "fragment_only": href.startswith("#"),
                "path": unquote(parsed.path),
            }
        )
    return result, diagnostics


def _text(value: Any) -> str:
    return value if isinstance(value, str) else ""


def _casefold(frontmatter: Mapping[str, Any], body: str):
    tags = frontmatter.get("tags")
    folded_tags = (
        sorted({item.casefold() for item in tags})
        if isinstance(tags, list) and all(isinstance(item, str) for item in tags)
        else []
    )
    type_name = _text(frontmatter.get("type"))
    title = _text(frontmatter.get("title"))
    description = _text(frontmatter.get("description"))
    return {
        "type": type_name.casefold(),
        "tags": folded_tags,
        "text": "\n".join((title, description, type_name, body)).casefold(),
    }


def parse_okf_bundle(root: Union[str, Path]) -> bytes:
    """Return canonical host-decoded OKF syntax for the native policy kernel."""

    _dependencies()
    root = Path(root)
    if not root.is_dir():
        raise ConfigError(f"OKF bundle is not a directory: {root}")
    paths, diagnostics = _bundle_paths(root)
    documents = []
    for file_path in paths:
        relative = file_path.relative_to(root).as_posix()
        try:
            raw = file_path.read_bytes()
        except OSError as error:
            diagnostics.append(_diagnostic("error", "read-failed", str(error), relative))
            continue
        if len(raw) > MAX_DOCUMENT_BYTES:
            diagnostics.append(
                _diagnostic(
                    "error",
                    "document-too-large",
                    f"document exceeds {MAX_DOCUMENT_BYTES} bytes",
                    relative,
                )
            )
            continue
        row: Dict[str, Any] = {
            "path": relative,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "byte_length": len(raw),
            "state": "invalid",
            "frontmatter": None,
            "body": None,
            "links": [],
            "casefold": None,
        }
        documents.append(row)
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            diagnostics.append(_diagnostic("error", "invalid-utf8", str(error), relative))
            continue
        if "\x00" in text:
            diagnostics.append(
                _diagnostic("error", "nul-byte", "document contains NUL", relative)
            )
            continue
        reserved = PurePosixPath(relative).name in RESERVED_NAMES
        try:
            frontmatter, body = _frontmatter(
                text,
                relative,
                required=not reserved,
                allowed=(not reserved or relative == "index.md"),
            )
        except ConfigError as error:
            diagnostics.append(
                _diagnostic("error", "invalid-document", str(error), relative)
            )
            continue
        links, link_diagnostics = _raw_links(body, relative)
        diagnostics.extend(link_diagnostics)
        row.update(
            state="current",
            frontmatter=frontmatter,
            body=body,
            links=links,
            casefold=_casefold(frontmatter, body),
        )
    document = {
        "schema_version": 1,
        "artifact": "okf-source-bundle",
        "producer": {
            "name": "archbird-python-okf",
            "version": __version__,
            "implementation_sha256": implementation_digest(),
            "runtime": f"cpython-{platform.python_version()}",
        },
        "known_paths": [path.relative_to(root).as_posix() for path in paths],
        "documents": documents,
        "diagnostics": sorted(
            {json.dumps(row, sort_keys=True, ensure_ascii=False): row for row in diagnostics}.values(),
            key=lambda row: (
                row["severity"],
                row["code"],
                row["message"],
                row["path"],
            ),
        ),
    }
    return json.dumps(
        document,
        sort_keys=True,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
    ).encode("utf-8")


def okf_query_input(
    *,
    concepts=(),
    types=(),
    tags=(),
    text=(),
    requirements=(),
) -> bytes:
    """Encode user selectors plus their host Unicode case-fold evidence."""

    document = {
        "schema_version": 1,
        "artifact": "okf-query-input",
        "concepts": list(concepts),
        "types": [{"value": value, "casefold": value.casefold()} for value in types],
        "tags": [{"value": value, "casefold": value.casefold()} for value in tags],
        "text": [{"value": value, "casefold": value.casefold()} for value in text],
        "requirements": list(requirements),
    }
    return json.dumps(
        document,
        sort_keys=True,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
    ).encode("utf-8")
