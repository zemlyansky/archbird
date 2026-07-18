"""Normalize optional SCIP reader evidence into the native provider fact IR."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
from typing import Dict, List, Mapping, Optional

from .reader import SCIPEvidence


def _framed(digest: "hashlib._Hash", value: bytes) -> None:
    digest.update(struct.pack(">Q", len(value)))
    digest.update(value)


def _fact_id(
    project: str,
    path: str,
    domain: str,
    kind: str,
    key: str,
) -> str:
    digest = hashlib.sha256()
    for value in (project, path, domain, kind):
        _framed(digest, value.encode("utf-8"))
    digest.update(struct.pack(">Q", 0))
    digest.update(struct.pack(">Q", 0))
    _framed(digest, key.encode("utf-8"))
    return "f:" + digest.hexdigest()


def _implementation_sha256() -> str:
    digest = hashlib.sha256()
    for path in (Path(__file__), Path(__file__).with_name("reader.py")):
        raw = path.read_bytes()
        _framed(digest, path.name.encode("utf-8"))
        _framed(digest, raw)
    return digest.hexdigest()


def _fact(
    *,
    project: str,
    path: str,
    domain: str,
    kind: str,
    key: str,
    name: Optional[str] = None,
    attributes: Optional[Mapping[str, object]] = None,
) -> Dict[str, object]:
    result: Dict[str, object] = {
        "claim": "compiler-index",
        "domain": domain,
        "id": _fact_id(project, path, domain, kind, key),
        "key": key,
        "kind": kind,
        "path": path,
        "project": project,
        "span": {"end": 0, "start": 0},
    }
    if name is not None:
        result["name"] = name
    if attributes:
        result["attributes"] = dict(attributes)
    return result


def scip_provider_facts(
    *,
    project: str,
    index_name: str,
    index_path: str,
    path_prefix: str,
    evidence: SCIPEvidence,
    source_manifest_sha256: str,
) -> bytes:
    """Return canonical project-scoped facts for one already-read SCIP index."""

    coverage = evidence.coverage
    summary = {
        "documents_mapped": coverage.documents_mapped,
        "documents_total": coverage.documents_total,
        "edge_count": len(evidence.edges),
        "format": "scip",
        "index_path": index_path,
        "occurrences": coverage.occurrences,
        "path_prefix": path_prefix,
        "references": coverage.references,
        "relationship_edges": coverage.relationship_edges,
        "relationships": coverage.relationships,
        "resolved_ambiguous": coverage.resolved_ambiguous,
        "resolved_unique": coverage.resolved_unique,
        "sha256": evidence.metadata.source_sha256,
        "symbols": coverage.symbols,
        "tool_name": evidence.metadata.tool_name,
        "tool_version": evidence.metadata.tool_version,
        "unresolved": coverage.unresolved,
    }
    facts: List[Dict[str, object]] = [
        _fact(
            project=project,
            path=index_path,
            domain="index-summaries",
            kind="summary",
            key=index_name,
            name=index_name,
            attributes=summary,
        )
    ]
    for edge in evidence.edges:
        for symbol in edge.symbols:
            key = json.dumps(
                [index_name, edge.kind, edge.source, edge.target, symbol],
                ensure_ascii=True,
                separators=(",", ":"),
            )
            facts.append(
                _fact(
                    project=project,
                    path=edge.source,
                    domain="index-edges",
                    kind=edge.kind,
                    key=key,
                    name=symbol,
                    attributes={
                        "index": index_name,
                        "source": edge.source,
                        "target": edge.target,
                    },
                )
            )
    for ordinal, diagnostic in enumerate(evidence.diagnostics):
        diagnostic_path = diagnostic.path or index_path
        code = "scip-" + diagnostic.code
        message = f"{index_name}: {diagnostic.message}"
        key = json.dumps(
            [index_name, ordinal, diagnostic.severity, code, message, diagnostic_path],
            ensure_ascii=True,
            separators=(",", ":"),
        )
        facts.append(
            _fact(
                project=project,
                path=index_path,
                domain="index-diagnostics",
                kind="diagnostic",
                key=key,
                attributes={
                    "code": code,
                    "diagnostic_path": diagnostic_path,
                    "index": index_name,
                    "message": message,
                    "severity": diagnostic.severity,
                },
            )
        )
    configuration = json.dumps(
        {
            "format": "scip",
            "index_name": index_name,
            "index_path": index_path,
            "path_prefix": path_prefix,
            "schema_version": 1,
        },
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    document = {
        "artifact": "archbird-provider-facts",
        "capabilities": [
            {
                "boundary": "diagnostics emitted by the pinned SCIP reader",
                "claims": ["compiler-index"],
                "coverage": "bounded",
                "domain": "index-diagnostics",
            },
            {
                "boundary": "unique mapped file edges resolved by the SCIP reader",
                "claims": ["compiler-index"],
                "coverage": "bounded",
                "domain": "index-edges",
            },
            {
                "boundary": "coverage counters for one complete supplied index",
                "claims": ["compiler-index"],
                "coverage": "complete",
                "domain": "index-summaries",
            },
        ],
        "diagnostics": [],
        "facts": sorted(facts, key=lambda item: str(item["id"])),
        "inputs": [
            {
                "project": project,
                "source_manifest_sha256": source_manifest_sha256,
            }
        ],
        "producer": {
            "configuration_sha256": hashlib.sha256(configuration).hexdigest(),
            "implementation_sha256": _implementation_sha256(),
            "name": "archbird-scip",
            "runtime": "python-protobuf-adapter",
            "version": "1",
        },
        "provenance": "derived",
        "resolutions": [],
        "schema_version": 1,
        "subject": {"project": project, "scope": "project"},
    }
    return json.dumps(
        document,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")


__all__ = ["scip_provider_facts"]
