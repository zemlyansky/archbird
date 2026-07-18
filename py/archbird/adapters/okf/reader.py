"""Python host facade over the shared native OKF policy kernel."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Sequence, Tuple, Union

from ...native import analyze_okf_source
from .parser import okf_query_input, parse_okf_bundle


@dataclass(frozen=True, order=True)
class OKFDiagnostic:
    severity: str
    code: str
    message: str
    path: str = ""


@dataclass(frozen=True, order=True)
class OKFLink:
    source: str
    label: str
    href: str
    target: str
    kind: str
    state: str


@dataclass(frozen=True, order=True)
class OKFRequirementLink:
    requirement_id: str
    concept_id: str
    source: str
    target_concept: str = ""


@dataclass
class OKFDocument:
    path: str
    concept_id: str
    kind: str
    sha256: str
    frontmatter: Mapping[str, Any]
    body: str
    type_name: str = ""
    title: str = ""
    description: str = ""
    resource: str = ""
    tags: Tuple[str, ...] = ()
    links: Tuple[OKFLink, ...] = ()
    explicit_requirements: Tuple[str, ...] = ()


@dataclass
class OKFIndex:
    bundle_sha256: str
    okf_version: str
    documents: Dict[str, OKFDocument]
    requirements: Tuple[OKFRequirementLink, ...]
    diagnostics: list[OKFDiagnostic] = field(default_factory=list)
    _source_bundle: bytes = field(default=b"", repr=False, compare=False)


def _document(row: Mapping[str, Any], requirements) -> OKFDocument:
    concept_id = str(row["concept_id"])
    path = str(row["path"])
    return OKFDocument(
        path=path,
        concept_id=concept_id,
        kind=str(row["kind"]),
        sha256=str(row["sha256"]),
        frontmatter=row["frontmatter"],
        body=str(row.get("body", "")),
        type_name=str(row["type"]),
        title=str(row["title"]),
        description=str(row["description"]),
        resource=str(row["resource"]),
        tags=tuple(str(value) for value in row["tags"]),
        links=tuple(
            OKFLink(
                source=path,
                label=str(link["label"]),
                href=str(link["href"]),
                target=str(link["target"]),
                kind=str(link["kind"]),
                state=str(link["state"]),
            )
            for link in row["links"]
        ),
        explicit_requirements=tuple(
            sorted(
                link.requirement_id
                for link in requirements
                if link.concept_id == concept_id and link.source == "explicit"
            )
        ),
    )


def _decode(encoded: bytes, source_bundle: bytes) -> OKFIndex:
    data = json.loads(encoded)
    requirements = tuple(
        OKFRequirementLink(
            requirement_id=str(row["requirement_id"]),
            concept_id=str(row["concept_id"]),
            source=str(row["source"]),
            target_concept=str(row["target_concept"]),
        )
        for row in data["requirement_links"]
    )
    documents = {
        str(row["path"]): _document(row, requirements) for row in data["documents"]
    }
    diagnostics = [
        OKFDiagnostic(
            severity=str(row["severity"]),
            code=str(row["code"]),
            message=str(row["message"]),
            path=str(row["path"]),
        )
        for row in data["diagnostics"]
    ]
    return OKFIndex(
        bundle_sha256=str(data["evidence"]["bundle_sha256"]),
        okf_version=str(data["evidence"]["okf_version"]),
        documents=dict(sorted(documents.items())),
        requirements=requirements,
        diagnostics=diagnostics,
        _source_bundle=source_bundle,
    )


def index_okf_bundle(root: Union[str, Path]) -> OKFIndex:
    """Decode host syntax, then apply shared native validation/index policy."""

    source = parse_okf_bundle(root)
    encoded = analyze_okf_source(
        source, format="json", include_body=True, pretty=False
    )
    return _decode(encoded, source)


def okf_has_errors(data: OKFIndex) -> bool:
    return any(row.severity == "error" for row in data.diagnostics)


def query_okf(
    data: OKFIndex,
    *,
    concepts: Sequence[str] = (),
    types: Sequence[str] = (),
    tags: Sequence[str] = (),
    text: Sequence[str] = (),
    requirements: Sequence[str] = (),
) -> Tuple[OKFDocument, ...]:
    query = okf_query_input(
        concepts=concepts,
        types=types,
        tags=tags,
        text=text,
        requirements=requirements,
    )
    encoded = analyze_okf_source(
        data._source_bundle,
        query_json=query,
        format="json",
        include_body=True,
        pretty=False,
    )
    selected = _decode(encoded, data._source_bundle)
    return tuple(sorted(selected.documents.values(), key=lambda row: row.concept_id))


def _literal_glob(value: str) -> str:
    return value.replace("[", "[[]").replace("*", "[*]").replace("?", "[?]")


def _selection_query(documents: Sequence[OKFDocument]) -> bytes:
    if not documents:
        raise ValueError("cannot render an empty OKF document selection")
    if any(row.kind != "concept" for row in documents):
        raise ValueError("OKF query output may contain concept documents only")
    return okf_query_input(
        concepts=[_literal_glob(row.concept_id) for row in documents]
    )


def render_okf_json(
    data: OKFIndex,
    *,
    documents: Optional[Sequence[OKFDocument]] = None,
    include_body: bool = False,
) -> str:
    query = b"" if documents is None else _selection_query(documents)
    return analyze_okf_source(
        data._source_bundle,
        query_json=query,
        format="json",
        include_body=include_body,
        pretty=True,
    ).decode("utf-8")


def render_okf_markdown(
    data: OKFIndex, *, documents: Optional[Sequence[OKFDocument]] = None
) -> str:
    query = b"" if documents is None else _selection_query(documents)
    return analyze_okf_source(
        data._source_bundle,
        query_json=query,
        format="markdown",
        include_body=True,
        pretty=False,
    ).decode("utf-8")
