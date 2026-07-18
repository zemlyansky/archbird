"""Streaming, optional reader for SCIP code-intelligence indexes."""

from __future__ import annotations

import hashlib
import json
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import (
    BinaryIO,
    DefaultDict,
    Iterable,
    Iterator,
    List,
    Mapping,
    Optional,
    Set,
    Tuple,
    Union,
)

from ...errors import ConfigError


MAX_FIELD_BYTES = 64 * 1024 * 1024
MAX_SYMBOL_BYTES = 16 * 1024
DEFINITION_ROLE = 1
IMPORT_ROLE = 2


@dataclass(frozen=True, order=True)
class SCIPDiagnostic:
    severity: str
    code: str
    message: str
    path: str = ""


@dataclass(frozen=True)
class SCIPMetadata:
    source_sha256: str
    protocol_version: int
    tool_name: str
    tool_version: str
    tool_arguments_sha256: str
    tool_argument_count: int
    project_root_sha256: str
    text_document_encoding: str


@dataclass(frozen=True, order=True)
class SCIPEdge:
    kind: str
    source: str
    target: str
    symbols: Tuple[str, ...]


@dataclass(frozen=True)
class SCIPCoverage:
    documents_total: int
    documents_mapped: int
    symbols: int
    occurrences: int
    references: int
    resolved_unique: int
    resolved_ambiguous: int
    unresolved: int
    relationships: int
    relationship_edges: int
    external_symbols: int


@dataclass
class SCIPEvidence:
    metadata: SCIPMetadata
    coverage: SCIPCoverage
    edges: Tuple[SCIPEdge, ...]
    diagnostics: List[SCIPDiagnostic] = field(default_factory=list)


def _protobuf():
    try:
        from google.protobuf.message import DecodeError  # type: ignore
    except ImportError as exc:
        raise ConfigError(
            "SCIP input requires the optional dependency; install archbird[scip]"
        ) from exc
    from . import scip_pb2

    return scip_pb2, DecodeError


def _source_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while True:
                block = handle.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    except OSError as exc:
        raise ConfigError(f"cannot read SCIP index {path}: {exc}") from exc
    return digest.hexdigest()


def _varint(handle: BinaryIO, *, allow_eof: bool = False) -> Optional[int]:
    value = 0
    shift = 0
    for index in range(10):
        byte = handle.read(1)
        if not byte:
            if allow_eof and index == 0:
                return None
            raise ConfigError("SCIP index: truncated protobuf varint")
        current = byte[0]
        value |= (current & 0x7F) << shift
        if not current & 0x80:
            return value
        shift += 7
    raise ConfigError("SCIP index: protobuf varint exceeds 10 bytes")


def _read_exact(handle: BinaryIO, size: int) -> bytes:
    if size < 0 or size > MAX_FIELD_BYTES:
        raise ConfigError(
            f"SCIP index: field length {size} exceeds {MAX_FIELD_BYTES} bytes"
        )
    result = handle.read(size)
    if len(result) != size:
        raise ConfigError("SCIP index: truncated protobuf field")
    return result


def _skip_field(handle: BinaryIO, wire: int) -> None:
    if wire == 0:
        _varint(handle)
    elif wire == 1:
        _read_exact(handle, 8)
    elif wire == 2:
        size = _varint(handle)
        assert size is not None
        _read_exact(handle, size)
    elif wire == 5:
        _read_exact(handle, 4)
    else:
        raise ConfigError(f"SCIP index: unsupported protobuf wire type {wire}")


def _index_fields(path: Path) -> Iterator[Tuple[int, bytes]]:
    try:
        handle = path.open("rb")
    except OSError as exc:
        raise ConfigError(f"cannot read SCIP index {path}: {exc}") from exc
    with handle:
        while True:
            key = _varint(handle, allow_eof=True)
            if key is None:
                return
            field = key >> 3
            wire = key & 7
            if field in {1, 2, 3}:
                if wire != 2:
                    raise ConfigError(
                        f"SCIP index: field {field} has invalid wire type {wire}"
                    )
                size = _varint(handle)
                assert size is not None
                yield field, _read_exact(handle, size)
            else:
                _skip_field(handle, wire)


def _parse_message(message, payload: bytes, label: str, DecodeError):
    try:
        message.ParseFromString(payload)
    except DecodeError as exc:
        raise ConfigError(f"SCIP index: invalid {label}: {exc}") from exc
    return message


def _metadata(path: Path) -> Tuple[SCIPMetadata, int]:
    scip_pb2, DecodeError = _protobuf()
    seen = 0
    external_symbols = 0
    parsed = None
    for field_number, payload in _index_fields(path):
        if field_number == 1:
            seen += 1
            if seen > 1:
                raise ConfigError("SCIP index: metadata must appear exactly once")
            parsed = _parse_message(
                scip_pb2.Metadata(), payload, "metadata", DecodeError
            )
        elif field_number == 2 and parsed is None:
            raise ConfigError("SCIP index: metadata must precede documents")
        elif field_number == 3:
            external_symbols += 1
    if parsed is None:
        raise ConfigError("SCIP index: missing metadata")
    arguments = list(parsed.tool_info.arguments)
    arguments_sha256 = hashlib.sha256(
        json.dumps(arguments, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    project_root = parsed.project_root.encode("utf-8")
    encoding = scip_pb2.TextEncoding.Name(parsed.text_document_encoding)
    return (
        SCIPMetadata(
            source_sha256=_source_sha256(path),
            protocol_version=int(parsed.version),
            tool_name=parsed.tool_info.name,
            tool_version=parsed.tool_info.version,
            tool_arguments_sha256=arguments_sha256,
            tool_argument_count=len(arguments),
            project_root_sha256=hashlib.sha256(project_root).hexdigest(),
            text_document_encoding=encoding,
        ),
        external_symbols,
    )


def _documents(path: Path):
    scip_pb2, DecodeError = _protobuf()
    for field_number, payload in _index_fields(path):
        if field_number == 2:
            yield _parse_message(scip_pb2.Document(), payload, "document", DecodeError)


def _canonical_path(value: str) -> bool:
    if not value or value.startswith("/") or "\\" in value:
        return False
    path = PurePosixPath(value)
    return path.as_posix() == value and all(
        part not in {"", ".", ".."} for part in path.parts
    )


def _prefixed_path(path: str, prefix: str) -> str:
    return f"{prefix}/{path}" if prefix else path


def _symbol_key(path: str, symbol: str) -> str:
    return f"{path}\0{symbol}" if symbol.startswith("local ") else symbol


def _valid_symbol(symbol: str) -> bool:
    return bool(symbol) and len(symbol.encode("utf-8")) <= MAX_SYMBOL_BYTES


def _relationship_kinds(relationship) -> Tuple[str, ...]:
    result = []
    if relationship.is_reference:
        result.append("scip-related-reference")
    if relationship.is_implementation:
        result.append("scip-implementation")
    if relationship.is_type_definition:
        result.append("scip-type-definition")
    if relationship.is_definition:
        result.append("scip-definition")
    return tuple(result)


def _aggregate_edges(
    rows: Mapping[Tuple[str, str, str], Set[str]],
) -> Tuple[SCIPEdge, ...]:
    return tuple(
        SCIPEdge(kind, source, target, tuple(sorted(symbols)))
        for (kind, source, target), symbols in sorted(rows.items())
    )


def analyze_scip_index(
    path: Union[str, Path],
    *,
    mapped_paths: Optional[Iterable[str]] = None,
    path_prefix: str = "",
) -> SCIPEvidence:
    """Derive file-reference evidence from a SCIP index in two streaming passes."""

    index_path = Path(path)
    if not index_path.is_file():
        raise ConfigError(f"SCIP index is not a file: {index_path}")
    if path_prefix and not _canonical_path(path_prefix):
        raise ConfigError("SCIP path_prefix must be canonical and repository-relative")
    metadata, external_symbols = _metadata(index_path)
    mapped = set(mapped_paths) if mapped_paths is not None else None
    diagnostics: List[SCIPDiagnostic] = []
    definitions: DefaultDict[str, Set[str]] = defaultdict(set)
    relationships: List[Tuple[str, str, str, Tuple[str, ...]]] = []
    seen_paths: Set[str] = set()
    duplicate_paths: Set[str] = set()
    documents_total = 0
    documents_mapped = 0
    symbol_count = 0
    occurrence_count = 0
    unspecified_positions = 0
    relationship_count = 0

    if not metadata.tool_name:
        diagnostics.append(
            SCIPDiagnostic("warning", "missing-tool-name", "indexer tool name is empty")
        )
    for document in _documents(index_path):
        documents_total += 1
        document_path = document.relative_path
        if not _canonical_path(document_path):
            diagnostics.append(
                SCIPDiagnostic(
                    "error",
                    "invalid-document-path",
                    "document path is not canonical and repository-relative",
                    document_path,
                )
            )
            continue
        path_value = _prefixed_path(document_path, path_prefix)
        if path_value in seen_paths:
            duplicate_paths.add(path_value)
            diagnostics.append(
                SCIPDiagnostic(
                    "error",
                    "duplicate-document",
                    "document path appears twice",
                    path_value,
                )
            )
            continue
        seen_paths.add(path_value)
        if not document.position_encoding:
            unspecified_positions += 1
        symbol_count += len(document.symbols)
        occurrence_count += len(document.occurrences)
        for information in document.symbols:
            if not _valid_symbol(information.symbol):
                diagnostics.append(
                    SCIPDiagnostic(
                        "error",
                        "invalid-symbol",
                        "symbol is empty or exceeds the configured byte limit",
                        path_value,
                    )
                )
                continue
            source_key = _symbol_key(path_value, information.symbol)
            definitions[source_key].add(path_value)
            for relationship in information.relationships:
                relationship_count += 1
                if not _valid_symbol(relationship.symbol):
                    diagnostics.append(
                        SCIPDiagnostic(
                            "error",
                            "invalid-relationship-symbol",
                            "relationship symbol is empty or too large",
                            path_value,
                        )
                    )
                    continue
                target_key = _symbol_key(path_value, relationship.symbol)
                kinds = _relationship_kinds(relationship)
                relationships.append(
                    (path_value, information.symbol, target_key, kinds)
                )
                if relationship.is_definition:
                    definitions[target_key].add(path_value)
        for occurrence in document.occurrences:
            if not occurrence.symbol_roles & DEFINITION_ROLE:
                continue
            if _valid_symbol(occurrence.symbol):
                definitions[_symbol_key(path_value, occurrence.symbol)].add(path_value)

    if unspecified_positions:
        diagnostics.append(
            SCIPDiagnostic(
                "warning",
                "unspecified-position-encoding",
                f"{unspecified_positions} documents omit position encoding",
            )
        )

    valid_paths = seen_paths - duplicate_paths
    documents_mapped = sum(
        mapped is None or path_value in mapped for path_value in valid_paths
    )
    if duplicate_paths:
        definitions = defaultdict(
            set,
            {
                key: candidates - duplicate_paths
                for key, candidates in definitions.items()
                if candidates - duplicate_paths
            },
        )
        relationships = [row for row in relationships if row[0] not in duplicate_paths]

    rows: DefaultDict[Tuple[str, str, str], Set[str]] = defaultdict(set)
    references = 0
    resolved_unique = 0
    resolved_ambiguous = 0
    unresolved = 0
    for document in _documents(index_path):
        if not _canonical_path(document.relative_path):
            continue
        source = _prefixed_path(document.relative_path, path_prefix)
        if source not in valid_paths:
            continue
        for occurrence in document.occurrences:
            symbol = occurrence.symbol
            if occurrence.symbol_roles & DEFINITION_ROLE or not _valid_symbol(symbol):
                continue
            references += 1
            candidates = definitions.get(_symbol_key(source, symbol), set())
            if not candidates:
                unresolved += 1
                continue
            if len(candidates) > 1:
                resolved_ambiguous += 1
                continue
            resolved_unique += 1
            target = next(iter(candidates))
            if source == target:
                continue
            if mapped is not None and (source not in mapped or target not in mapped):
                continue
            kind = (
                "scip-import"
                if occurrence.symbol_roles & IMPORT_ROLE
                else "scip-reference"
            )
            rows[(kind, source, target)].add(symbol)

    relationship_edges = 0
    for source, symbol, target_key, kinds in relationships:
        candidates = definitions.get(target_key, set())
        if len(candidates) != 1:
            continue
        target = next(iter(candidates))
        if source == target:
            continue
        if mapped is not None and (source not in mapped or target not in mapped):
            continue
        for kind in kinds:
            rows[(kind, source, target)].add(symbol)
            relationship_edges += 1

    return SCIPEvidence(
        metadata=metadata,
        coverage=SCIPCoverage(
            documents_total=documents_total,
            documents_mapped=documents_mapped,
            symbols=symbol_count,
            occurrences=occurrence_count,
            references=references,
            resolved_unique=resolved_unique,
            resolved_ambiguous=resolved_ambiguous,
            unresolved=unresolved,
            relationships=relationship_count,
            relationship_edges=relationship_edges,
            external_symbols=external_symbols,
        ),
        edges=_aggregate_edges(rows),
        diagnostics=sorted(set(diagnostics)),
    )
