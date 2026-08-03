"""Bounded MCP stdio access to one live Archbird repository."""

from __future__ import annotations

from collections import OrderedDict
import hashlib
import json
import re
import sys
from typing import Any, BinaryIO, Mapping, Optional

from . import __version__
from .native import (
    diff_maps_json,
    evaluate_projection_json,
    path_map_json,
    path_map_markdown,
    query_map_markdown,
    render_map_markdown,
)
from .serve import BODY_LIMIT, LiveRepository


PROTOCOL_VERSION = "2025-11-25"
SUPPORTED_PROTOCOL_VERSIONS = {
    "2024-11-05",
    "2025-03-26",
    "2025-06-18",
    PROTOCOL_VERSION,
}
RESULT_LIMIT = 2 * 1024 * 1024
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def _object_schema(
    properties: Mapping[str, object],
    *,
    required: tuple[str, ...] = (),
) -> dict[str, object]:
    result: dict[str, object] = {
        "additionalProperties": False,
        "properties": dict(properties),
        "type": "object",
    }
    if required:
        result["required"] = list(required)
    return result


GENERATION = {
    "description": "Optional exact live Map generation SHA-256.",
    "pattern": "^[0-9a-f]{64}$",
    "type": "string",
}
STRINGS = {"items": {"type": "string"}, "type": "array"}
SELECTOR_STRINGS = {
    "items": {"minLength": 1, "type": "string"},
    "minItems": 1,
    "type": "array",
}
READ_ONLY = {
    "destructiveHint": False,
    "idempotentHint": True,
    "openWorldHint": False,
    "readOnlyHint": True,
}
TOOLS = [
    {
        "annotations": READ_ONLY,
        "description": "Return the current live repository generation and analysis state.",
        "inputSchema": _object_schema({}),
        "name": "archbird_status",
        "title": "Archbird repository status",
    },
    {
        "annotations": READ_ONLY,
        "description": (
            "Render a bounded architecture-first report from one canonical live Map."
        ),
        "inputSchema": _object_schema(
            {
                "detail": {
                    "enum": ["compact", "standard", "full"],
                    "type": "string",
                },
                "generation": GENERATION,
                "group_by": {
                    "enum": ["component", "directory", "language", "layer", "none"],
                    "type": "string",
                },
                "level": {
                    "enum": ["component", "file", "symbol"],
                    "type": "string",
                },
                "max_chars": {
                    "maximum": 100000,
                    "minimum": 1,
                    "type": "integer",
                },
                "overlays": STRINGS,
                "relations": STRINGS,
                "view": {
                    "enum": ["overview", "architecture", "tests", "evidence"],
                    "type": "string",
                },
            }
        ),
        "name": "archbird_map",
        "title": "Map repository architecture",
    },
    {
        "annotations": READ_ONLY,
        "description": (
            "Evaluate one exhaustive typed ProjectionPlan over the current Map. "
            "The result is never ranked or truncated."
        ),
        "inputSchema": _object_schema(
            {
                "generation": GENERATION,
                "plan": {"type": "object"},
            },
            required=("plan",),
        ),
        "name": "archbird_projection",
        "title": "Evaluate an Archbird projection",
    },
    {
        "annotations": READ_ONLY,
        "description": (
            "Run a focused deterministic Query over the current Map and return "
            "bounded Markdown context."
        ),
        "inputSchema": _object_schema(
            {
                "artifacts": SELECTOR_STRINGS,
                "components": SELECTOR_STRINGS,
                "depth": {"maximum": 32, "minimum": 0, "type": "integer"},
                "detail": {
                    "enum": ["compact", "standard", "full"],
                    "type": "string",
                },
                "direction": {
                    "enum": ["both", "downstream", "upstream"],
                    "type": "string",
                },
                "focus": SELECTOR_STRINGS,
                "generation": GENERATION,
                "max_chars": {
                    "maximum": 100000,
                    "minimum": 1,
                    "type": "integer",
                },
                "packages": SELECTOR_STRINGS,
                "paths": SELECTOR_STRINGS,
                "search": SELECTOR_STRINGS,
                "search_limit": {
                    "maximum": 100,
                    "minimum": 1,
                    "type": "integer",
                },
                "symbols": SELECTOR_STRINGS,
                "test_depth": {"maximum": 32, "minimum": 0, "type": "integer"},
            }
        )
        | {
            "anyOf": [
                {"required": [name]}
                for name in (
                    "artifacts",
                    "components",
                    "focus",
                    "packages",
                    "paths",
                    "search",
                    "symbols",
                )
            ]
        },
        "name": "archbird_query",
        "title": "Query repository evidence",
    },
    {
        "annotations": READ_ONLY,
        "description": (
            "Find bounded shortest connection witnesses over typed Map "
            "relations while retaining endpoint ambiguity and evidence."
        ),
        "inputSchema": _object_schema(
            {
                "direction": {
                    "enum": ["both", "downstream", "upstream"],
                    "type": "string",
                },
                "generation": GENERATION,
                "level": {
                    "enum": ["component", "file", "symbol"],
                    "type": "string",
                },
                "max_depth": {
                    "maximum": 64,
                    "minimum": 0,
                    "type": "integer",
                },
                "max_paths": {
                    "maximum": 32,
                    "minimum": 1,
                    "type": "integer",
                },
                "relations": STRINGS,
                "source": {"minLength": 1, "type": "string"},
                "source_kind": {"minLength": 1, "type": "string"},
                "target": {"minLength": 1, "type": "string"},
                "target_kind": {"minLength": 1, "type": "string"},
            },
            required=("source", "target"),
        ),
        "name": "archbird_path",
        "title": "Find repository connection paths",
    },
    {
        "annotations": READ_ONLY,
        "description": (
            "Read one hash-checked source file from the selected Map generation."
        ),
        "inputSchema": _object_schema(
            {"generation": GENERATION, "path": {"minLength": 1, "type": "string"}},
            required=("path",),
        ),
        "name": "archbird_source",
        "title": "Read mapped source",
    },
    {
        "annotations": READ_ONLY,
        "description": (
            "Return the exhaustive Verification artifact for the selected "
            "generation. Returns an error when no constraints are configured."
        ),
        "inputSchema": _object_schema({"generation": GENERATION}),
        "name": "archbird_verify",
        "title": "Verify architecture constraints",
    },
    {
        "annotations": READ_ONLY,
        "description": "Diff two retained canonical Map generations.",
        "inputSchema": _object_schema(
            {
                "after": GENERATION,
                "before": GENERATION,
            },
            required=("before", "after"),
        ),
        "name": "archbird_diff",
        "title": "Diff Map generations",
    },
]


class McpError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


def _strict_json(data: bytes) -> object:
    def constant(value: str) -> object:
        raise ValueError(f"non-finite JSON number {value}")

    def pairs(values: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in values:
            if key in result:
                raise ValueError(f"duplicate JSON field {key!r}")
            result[key] = value
        return result

    return json.loads(data, parse_constant=constant, object_pairs_hook=pairs)


def _arguments(value: object, allowed: set[str]) -> dict[str, object]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise McpError(-32602, "tool arguments must be an object")
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise McpError(-32602, f"unknown tool argument: {unknown[0]}")
    return value


def _generation(arguments: Mapping[str, object]) -> Optional[str]:
    value = arguments.get("generation")
    if value is None:
        return None
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise McpError(-32602, "generation must be a lowercase SHA-256 digest")
    return value


def _integer(
    arguments: Mapping[str, object],
    name: str,
    default: int,
    minimum: int,
    maximum: int,
) -> int:
    value = arguments.get(name, default)
    if isinstance(value, bool) or not isinstance(value, int):
        raise McpError(-32602, f"{name} must be an integer")
    if value < minimum or value > maximum:
        raise McpError(-32602, f"{name} must be from {minimum} to {maximum}")
    return value


def _string(
    arguments: Mapping[str, object],
    name: str,
    default: str,
    allowed: set[str],
) -> str:
    value = arguments.get(name, default)
    if not isinstance(value, str) or value not in allowed:
        raise McpError(-32602, f"{name} must be one of {', '.join(sorted(allowed))}")
    return value


def _strings(arguments: Mapping[str, object], name: str) -> tuple[str, ...]:
    value = arguments.get(name, ())
    if not isinstance(value, (list, tuple)) or any(
        not isinstance(item, str) or not item for item in value
    ):
        raise McpError(-32602, f"{name} must contain non-empty strings")
    return tuple(value)


def _result(
    value: object,
    text: Optional[str] = None,
    resource: Optional[Mapping[str, object]] = None,
) -> dict[str, object]:
    serialized = json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    )
    rendered = serialized if text is None else text
    if len(serialized.encode("utf-8")) + len(rendered.encode("utf-8")) > RESULT_LIMIT:
        raise ValueError(
            f"tool result exceeds {RESULT_LIMIT} bytes; narrow the projection or query"
        )
    content: list[dict[str, object]] = [{"text": rendered, "type": "text"}]
    if resource is not None:
        content.append(
            {
                "description": resource["description"],
                "mimeType": resource["mimeType"],
                "name": resource["name"],
                "type": "resource_link",
                "uri": resource["uri"],
            }
        )
    return {
        "content": content,
        "isError": False,
        "structuredContent": value,
    }


class ArchbirdMcpServer:
    def __init__(self, repository: LiveRepository) -> None:
        self.repository = repository
        self.initialized = False
        self.resources: "OrderedDict[str, tuple[dict[str, object], bytes]]" = (
            OrderedDict()
        )

    def _remember_resource(
        self,
        snapshot: Mapping[str, object],
        kind: str,
        data: bytes,
        *,
        description: str,
        mime_type: str,
        title: str,
    ) -> dict[str, object]:
        digest = hashlib.sha256(data).hexdigest()
        uri = (
            f"archbird://repository/{snapshot['generation']}/{kind}/{digest}"
        )
        descriptor = {
            "description": description,
            "mimeType": mime_type,
            "name": f"{kind}-{digest[:12]}",
            "size": len(data),
            "title": title,
            "uri": uri,
        }
        self.resources.pop(uri, None)
        self.resources[uri] = (descriptor, bytes(data))
        while len(self.resources) > 32:
            self.resources.popitem(last=False)
        return descriptor

    def _base_resources(self) -> None:
        for snapshot in self.repository.retained_snapshots():
            self._remember_resource(
                snapshot,
                "map",
                snapshot["map"],
                description="Canonical Map for one retained repository generation.",
                mime_type="application/json",
                title=f"{snapshot['project']} Map",
            )
            if snapshot["verification"]:
                self._remember_resource(
                    snapshot,
                    "verification",
                    snapshot["verification"],
                    description=(
                        "Exhaustive Verification result for one retained Map "
                        "generation."
                    ),
                    mime_type="application/json",
                    title=f"{snapshot['project']} Verification",
                )

    def _snapshot(self, arguments: Mapping[str, object]) -> dict[str, Any]:
        return self.repository.snapshot(_generation(arguments))

    def _call_tool(self, name: object, raw_arguments: object) -> dict[str, object]:
        if not isinstance(name, str):
            raise McpError(-32602, "tools/call requires a tool name")
        if name == "archbird_status":
            arguments = _arguments(raw_arguments, set())
            return _result(self.repository.state())
        if name == "archbird_map":
            arguments = _arguments(
                raw_arguments,
                {
                    "detail", "generation", "group_by", "level", "max_chars",
                    "overlays", "relations", "view",
                },
            )
            snapshot = self._snapshot(arguments)
            view = _string(
                arguments,
                "view",
                "overview",
                {"architecture", "evidence", "overview", "tests"},
            )
            detail = _string(
                arguments,
                "detail",
                "standard",
                {"compact", "full", "standard"},
            )
            group_by = _string(
                arguments,
                "group_by",
                "",
                {"", "component", "directory", "language", "layer", "none"},
            )
            level = _string(
                arguments,
                "level",
                "",
                {"", "component", "file", "symbol"},
            )
            relations = (
                _strings(arguments, "relations")
                if "relations" in arguments
                else None
            )
            overlays = (
                _strings(arguments, "overlays")
                if "overlays" in arguments
                else None
            )
            max_chars = _integer(arguments, "max_chars", 12000, 1, 100000)
            request = {
                "detail": detail,
                "group_by": group_by,
                "level": level,
                "max_chars": max_chars,
                "overlays": overlays,
                "relations": relations,
                "view": view,
            }
            report = self.repository.derived(
                snapshot,
                "mcp-map",
                request,
                lambda: render_map_markdown(
                    snapshot["map"],
                    view=view,
                    detail=detail,
                    max_chars=max_chars,
                    group_by=group_by,
                    level=level,
                    relations=relations,
                    overlays=overlays,
                ),
            ).decode("utf-8")
            resource = self._remember_resource(
                snapshot,
                "map-report",
                report.encode("utf-8"),
                description="Bounded Markdown presentation of a canonical Map.",
                mime_type="text/markdown",
                title=f"{snapshot['project']} architecture report",
            )
            return _result(
                {
                    "artifact": "archbird-map-report",
                    "generation": snapshot["generation"],
                    "project": snapshot["project"],
                    "text": report,
                },
                report,
                resource,
            )
        if name == "archbird_projection":
            arguments = _arguments(raw_arguments, {"generation", "plan"})
            plan = arguments.get("plan")
            if not isinstance(plan, dict):
                raise McpError(-32602, "plan must be an object")
            snapshot = self._snapshot(arguments)
            encoded = self.repository.derived(
                snapshot,
                "mcp-projection",
                plan,
                lambda: evaluate_projection_json(snapshot["map"], plan),
            )
            if len(encoded) > RESULT_LIMIT:
                raise ValueError(
                    f"projection result exceeds {RESULT_LIMIT} bytes; narrow its domain"
                )
            resource = self._remember_resource(
                snapshot,
                "projection",
                encoded,
                description="Exhaustive typed ProjectionResult.",
                mime_type="application/json",
                title=f"{snapshot['project']} projection",
            )
            return _result(_strict_json(encoded), resource=resource)
        if name == "archbird_query":
            allowed = {
                "artifacts", "components", "depth", "detail", "direction",
                "focus", "generation", "max_chars", "packages", "paths",
                "search", "search_limit", "symbols", "test_depth",
            }
            arguments = _arguments(raw_arguments, allowed)
            snapshot = self._snapshot(arguments)
            options = {
                key: _strings(arguments, key)
                for key in (
                    "artifacts", "components", "focus", "packages", "paths",
                    "search", "symbols",
                )
            }
            if not any(options.values()):
                raise McpError(
                    -32602,
                    "archbird_query requires at least one non-empty selector",
                )
            direction = _string(
                arguments,
                "direction",
                "both",
                {"both", "downstream", "upstream"},
            )
            detail = _string(
                arguments,
                "detail",
                "standard",
                {"compact", "full", "standard"},
            )
            max_chars = _integer(arguments, "max_chars", 12000, 1, 100000)
            depth = _integer(arguments, "depth", 1, 0, 32)
            test_depth = _integer(arguments, "test_depth", 8, 0, 32)
            search_limit = _integer(arguments, "search_limit", 8, 1, 100)
            request = {
                **options,
                "depth": depth,
                "detail": detail,
                "direction": direction,
                "max_chars": max_chars,
                "search_limit": search_limit,
                "test_depth": test_depth,
            }
            report = self.repository.derived(
                snapshot,
                "mcp-query",
                request,
                lambda: query_map_markdown(
                    snapshot["map"],
                    **options,
                    direction=direction,
                    depth=depth,
                    test_depth=test_depth,
                    search_limit=search_limit,
                    detail=detail,
                    max_chars=max_chars,
                ),
            ).decode("utf-8")
            resource = self._remember_resource(
                snapshot,
                "query-report",
                report.encode("utf-8"),
                description="Bounded focused Query report.",
                mime_type="text/markdown",
                title=f"{snapshot['project']} Query",
            )
            return _result(
                {
                    "artifact": "archbird-query-report",
                    "generation": snapshot["generation"],
                    "project": snapshot["project"],
                    "text": report,
                },
                report,
                resource,
            )
        if name == "archbird_path":
            allowed = {
                "direction",
                "generation",
                "level",
                "max_depth",
                "max_paths",
                "relations",
                "source",
                "source_kind",
                "target",
                "target_kind",
            }
            arguments = _arguments(raw_arguments, allowed)
            source_pattern = arguments.get("source")
            target_pattern = arguments.get("target")
            if not isinstance(source_pattern, str) or not source_pattern:
                raise McpError(-32602, "source must be a non-empty string")
            if not isinstance(target_pattern, str) or not target_pattern:
                raise McpError(-32602, "target must be a non-empty string")
            snapshot = self._snapshot(arguments)
            level = _string(
                arguments,
                "level",
                "file",
                {"component", "file", "symbol"},
            )
            direction = _string(
                arguments,
                "direction",
                "downstream",
                {"both", "downstream", "upstream"},
            )
            max_depth = _integer(arguments, "max_depth", 8, 0, 64)
            max_paths = _integer(arguments, "max_paths", 8, 1, 32)
            relations = (
                _strings(arguments, "relations")
                if "relations" in arguments
                else None
            )
            source_kind = arguments.get("source_kind", level)
            target_kind = arguments.get("target_kind", level)
            if not isinstance(source_kind, str) or not source_kind:
                raise McpError(-32602, "source_kind must be a non-empty string")
            if not isinstance(target_kind, str) or not target_kind:
                raise McpError(-32602, "target_kind must be a non-empty string")
            request = {
                "direction": direction,
                "level": level,
                "max_depth": max_depth,
                "max_paths": max_paths,
                "relations": relations,
                "source": source_pattern,
                "source_kind": source_kind,
                "target": target_pattern,
                "target_kind": target_kind,
            }
            encoded = self.repository.derived(
                snapshot,
                "mcp-path",
                request,
                lambda: path_map_json(
                    snapshot["map"],
                    {"kind": source_kind, "patterns": [source_pattern]},
                    {"kind": target_kind, "patterns": [target_pattern]},
                    level=level,
                    relations=relations,
                    direction=direction,
                    max_depth=max_depth,
                    max_paths=max_paths,
                ),
            )
            if len(encoded) > RESULT_LIMIT:
                raise ValueError(
                    f"path result exceeds {RESULT_LIMIT} bytes; lower max_paths"
                )
            report = path_map_markdown(
                snapshot["map"],
                {"kind": source_kind, "patterns": [source_pattern]},
                {"kind": target_kind, "patterns": [target_pattern]},
                level=level,
                relations=relations,
                direction=direction,
                max_depth=max_depth,
                max_paths=max_paths,
            ).decode("utf-8")
            resource = self._remember_resource(
                snapshot,
                "path",
                encoded,
                description="Canonical evidence-preserving Path artifact.",
                mime_type="application/json",
                title=f"{snapshot['project']} connection paths",
            )
            return _result(_strict_json(encoded), report, resource)
        if name == "archbird_source":
            arguments = _arguments(raw_arguments, {"generation", "path"})
            path = arguments.get("path")
            if not isinstance(path, str) or not path:
                raise McpError(-32602, "path must be a non-empty string")
            snapshot = self._snapshot(arguments)
            source = self.repository.source(snapshot, path)
            encoded = json.dumps(
                source, ensure_ascii=True, separators=(",", ":"), sort_keys=True
            ).encode("utf-8")
            resource = self._remember_resource(
                snapshot,
                "source",
                encoded,
                description="Hash-checked source response bound to a Map generation.",
                mime_type="application/json",
                title=path,
            )
            return _result(source, resource=resource)
        if name == "archbird_verify":
            arguments = _arguments(raw_arguments, {"generation"})
            snapshot = self._snapshot(arguments)
            if not snapshot["verification"]:
                raise ValueError("no architecture constraints are configured")
            encoded = snapshot["verification"]
            if len(encoded) > RESULT_LIMIT:
                raise ValueError(
                    f"verification result exceeds {RESULT_LIMIT} bytes"
                )
            resource = self._remember_resource(
                snapshot,
                "verification",
                encoded,
                description="Exhaustive Verification result.",
                mime_type="application/json",
                title=f"{snapshot['project']} Verification",
            )
            return _result(_strict_json(encoded), resource=resource)
        if name == "archbird_diff":
            arguments = _arguments(raw_arguments, {"after", "before"})
            before = arguments.get("before")
            after = arguments.get("after")
            if (
                not isinstance(before, str)
                or not SHA256.fullmatch(before)
                or not isinstance(after, str)
                or not SHA256.fullmatch(after)
            ):
                raise McpError(
                    -32602, "before and after must be lowercase SHA-256 digests"
                )
            before_snapshot = self.repository.snapshot(before)
            after_snapshot = self.repository.snapshot(after)
            encoded = self.repository.derived(
                after_snapshot,
                "mcp-diff",
                {"after": after, "before": before},
                lambda: diff_maps_json(
                    before_snapshot["map"], after_snapshot["map"]
                ),
            )
            if len(encoded) > RESULT_LIMIT:
                raise ValueError(f"diff result exceeds {RESULT_LIMIT} bytes")
            resource = self._remember_resource(
                after_snapshot,
                "diff",
                encoded,
                description="Structural Diff between two retained Map generations.",
                mime_type="application/json",
                title=f"{after_snapshot['project']} Diff",
            )
            return _result(_strict_json(encoded), resource=resource)
        raise McpError(-32602, f"unknown tool {name!r}")

    def dispatch(self, request: Mapping[str, object]) -> Optional[dict[str, object]]:
        identity = request.get("id")
        method = request.get("method")
        notification = "id" not in request
        if request.get("jsonrpc") != "2.0" or not isinstance(method, str):
            raise McpError(-32600, "invalid JSON-RPC request")
        if method == "notifications/initialized":
            return None
        if method == "notifications/cancelled":
            return None
        if notification:
            return None
        if identity is None or isinstance(identity, bool) or not isinstance(
            identity, (int, str)
        ):
            raise McpError(-32600, "request id must be a string or integer")
        params = request.get("params", {})
        if not isinstance(params, dict):
            raise McpError(-32602, "request params must be an object")
        if method == "initialize":
            requested = params.get("protocolVersion")
            capabilities = params.get("capabilities")
            client = params.get("clientInfo")
            if not isinstance(requested, str) or not requested:
                raise McpError(
                    -32602, "initialize requires a protocolVersion string"
                )
            if not isinstance(capabilities, dict):
                raise McpError(
                    -32602, "initialize requires client capabilities"
                )
            if (
                not isinstance(client, dict)
                or not isinstance(client.get("name"), str)
                or not client["name"]
                or not isinstance(client.get("version"), str)
                or not client["version"]
            ):
                raise McpError(
                    -32602,
                    "initialize requires clientInfo name and version strings",
                )
            selected = (
                requested
                if requested in SUPPORTED_PROTOCOL_VERSIONS
                else PROTOCOL_VERSION
            )
            self.initialized = True
            return {
                "capabilities": {
                    "resources": {"listChanged": False, "subscribe": False},
                    "tools": {"listChanged": False},
                },
                "instructions": (
                    "Use archbird_map for orientation, archbird_query for focused "
                    "context, archbird_path for typed connection witnesses, "
                    "archbird_projection for exhaustive typed facts, and "
                    "archbird_verify before architecture-sensitive changes."
                ),
                "protocolVersion": selected,
                "serverInfo": {
                    "description": "Architecture evidence for one live repository",
                    "name": "archbird",
                    "title": "Archbird",
                    "version": __version__,
                },
            }
        if method == "ping":
            return {}
        if not self.initialized:
            raise McpError(-32002, "server is not initialized")
        if method == "tools/list":
            if params.get("cursor") is not None:
                raise McpError(-32602, "tools/list cursor is invalid")
            return {"tools": TOOLS}
        if method == "tools/call":
            return self._call_tool(params.get("name"), params.get("arguments", {}))
        if method == "resources/list":
            if params.get("cursor") is not None:
                raise McpError(-32602, "resources/list cursor is invalid")
            self._base_resources()
            return {
                "resources": [
                    descriptor
                    for descriptor, _data in self.resources.values()
                ]
            }
        if method == "resources/read":
            uri = params.get("uri")
            if not isinstance(uri, str):
                raise McpError(-32602, "resources/read requires a URI")
            self._base_resources()
            selected = self.resources.get(uri)
            if selected is None:
                raise McpError(-32602, "unknown Archbird resource URI")
            descriptor, data = selected
            if len(data) > RESULT_LIMIT:
                raise McpError(
                    -32603,
                    f"resource exceeds {RESULT_LIMIT} bytes; use a projection or query",
                )
            try:
                text = data.decode("utf-8", errors="strict")
            except UnicodeDecodeError as error:
                raise McpError(-32603, "resource is not UTF-8 text") from error
            return {
                "contents": [
                    {
                        "mimeType": descriptor["mimeType"],
                        "text": text,
                        "uri": uri,
                    }
                ]
            }
        raise McpError(-32601, f"method not found: {method}")


def _write(output: BinaryIO, value: Mapping[str, object]) -> None:
    output.write(
        json.dumps(
            value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
        + b"\n"
    )
    output.flush()


def serve_stdio(
    repository: LiveRepository,
    *,
    input: Optional[BinaryIO] = None,
    output: Optional[BinaryIO] = None,
) -> int:
    source = input if input is not None else sys.stdin.buffer
    target = output if output is not None else sys.stdout.buffer
    server = ArchbirdMcpServer(repository)
    while True:
        line = source.readline(BODY_LIMIT + 2)
        if not line:
            return 0
        identity: object = None
        request: object = None
        try:
            if len(line) > BODY_LIMIT or not line.endswith(b"\n"):
                raise McpError(-32700, f"MCP message exceeds {BODY_LIMIT} bytes")
            request = _strict_json(line)
            if not isinstance(request, dict):
                raise McpError(-32600, "JSON-RPC request must be an object")
            identity = request.get("id")
            result = server.dispatch(request)
            if result is None:
                continue
            _write(target, {"id": identity, "jsonrpc": "2.0", "result": result})
        except McpError as error:
            _write(
                target,
                {
                    "error": {"code": error.code, "message": str(error)},
                    "id": identity,
                    "jsonrpc": "2.0",
                },
            )
        except (UnicodeDecodeError, ValueError, json.JSONDecodeError, RuntimeError) as error:
            if isinstance(request, dict) and "id" not in request:
                continue
            if isinstance(request, dict) and request.get("method") == "tools/call":
                _write(
                    target,
                    {
                        "id": identity,
                        "jsonrpc": "2.0",
                        "result": {
                            "content": [{"text": str(error), "type": "text"}],
                            "isError": True,
                        },
                    },
                )
            else:
                code = -32700 if request is None else -32603
                _write(
                    target,
                    {
                        "error": {"code": code, "message": str(error)},
                        "id": identity,
                        "jsonrpc": "2.0",
                    },
                )
