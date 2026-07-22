"""ctypes source-checkout adapter for the generic ``libarchbird`` C ABI.

Wheels provide the compiled ``archbird._native`` extension, which Python loads
before this module.  A repository checkout deliberately uses this file and the
root ``build/libarchbird`` shared library, matching the Polygrad/Tranfi native
development model without installing or copying a private Python environment.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path
from typing import Callable, Iterable, Optional, Sequence


class Error(RuntimeError):
    """Native Archbird failure."""

    def __init__(
        self, message: str, *, status: int | None = None, offset: int | None = None
    ) -> None:
        super().__init__(message)
        self.status = status
        self.offset = offset


NATIVE_ABI_VERSION = 0
PATTERN_CONTRACT_VERSION = 1
PATTERN_CONTRACT = "archbird-pcre2-v1"
PATTERN_ENGINE = "PCRE2 10.47"
PATTERN_UNICODE = "UCD 16.0.0"
PATTERN_OPTIONS = (
    "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,NEVER_CALLOUT,"
    "JIT_DISABLED"
)

_OK = 0
_INVALID_ARGUMENT = 1
_JSON_PRETTY = 1
_JSON_TRAILING_NEWLINE = 2
_SIZE_MAX = ctypes.c_size_t(-1).value
_POINTER = ctypes.c_void_p
_BYTES_POINTER = ctypes.POINTER(ctypes.c_uint8)
_WRITE = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, _BYTES_POINTER, ctypes.c_size_t
)


def _library_candidates() -> Iterable[Path | str]:
    override = os.environ.get("ARCHBIRD_LIB")
    if override:
        yield Path(override).expanduser()
    package = Path(__file__).resolve().parent
    repository = package.parents[1]
    for name in ("libarchbird.so", "libarchbird.dylib", "archbird.dll"):
        yield repository / "build" / name
        yield package / name
    found = ctypes.util.find_library("archbird")
    if found:
        yield found


def _load_library() -> ctypes.CDLL:
    failures = []
    for candidate in _library_candidates():
        path = str(candidate)
        if isinstance(candidate, Path) and not candidate.is_file():
            failures.append(f"{path}: missing")
            continue
        try:
            return ctypes.CDLL(path)
        except OSError as error:
            failures.append(f"{path}: {error}")
    detail = "\n".join(failures)
    raise ImportError(
        "the local Archbird C core is not built; run `make build-c`"
        + (f"\n{detail}" if detail else "")
    )


_lib = _load_library()


class _MergeSummary(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("providers", ctypes.c_size_t),
        ("selections", ctypes.c_size_t),
        ("selected_facts", ctypes.c_size_t),
        ("contributed", ctypes.c_size_t),
        ("deduplicated", ctypes.c_size_t),
        ("enriched", ctypes.c_size_t),
        ("variations", ctypes.c_size_t),
        ("conflicts", ctypes.c_size_t),
        ("audit_matches", ctypes.c_size_t),
        ("audit_differences", ctypes.c_size_t),
    ]


class _GraphOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("format", ctypes.c_int),
        ("view", ctypes.c_int),
        ("direction", ctypes.c_int),
        ("max_nodes", ctypes.c_size_t),
        ("max_edge_names", ctypes.c_size_t),
    ]


class _EngineOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("max_input_bytes", ctypes.c_size_t),
        ("max_depth", ctypes.c_size_t),
        ("max_values", ctypes.c_size_t),
        ("max_string_bytes", ctypes.c_size_t),
        ("max_files", ctypes.c_size_t),
        ("max_file_bytes", ctypes.c_size_t),
        ("max_index_bytes", ctypes.c_size_t),
        ("max_source_bytes", ctypes.c_size_t),
        ("max_syntax_bytes", ctypes.c_size_t),
        ("max_provider_bundles", ctypes.c_size_t),
        ("max_facts", ctypes.c_size_t),
        ("max_pattern_matches", ctypes.c_size_t),
        ("regex_match_limit", ctypes.c_uint32),
        ("regex_depth_limit", ctypes.c_uint32),
        ("regex_heap_limit_kib", ctypes.c_uint32),
        ("allocate", _POINTER),
        ("reallocate", _POINTER),
        ("deallocate", _POINTER),
        ("allocator_user_data", _POINTER),
    ]


def _declare(name: str, arguments: Sequence[object], result=ctypes.c_int):
    function = getattr(_lib, name)
    function.argtypes = list(arguments)
    function.restype = result
    return function


_engine_create = _declare(
    "archbird_engine_create", [_POINTER, ctypes.POINTER(_POINTER)]
)
_engine_options_init = _declare(
    "archbird_engine_options_init", [ctypes.POINTER(_EngineOptions)], None
)
_engine_options_init_for_input = _declare(
    "archbird_engine_options_init_for_input",
    [ctypes.POINTER(_EngineOptions), ctypes.c_int, ctypes.c_size_t],
)
_engine_destroy = _declare("archbird_engine_destroy", [_POINTER], None)
_engine_error = _declare("archbird_engine_error", [_POINTER], ctypes.c_char_p)
_engine_error_offset = _declare(
    "archbird_engine_error_offset", [_POINTER], ctypes.c_size_t
)
_implementation = _declare(
    "archbird_implementation_sha256", [], ctypes.c_char_p
)
_project_destroy = _declare("archbird_project_destroy", [_POINTER], None)
_discovery_destroy = _declare("archbird_discovery_destroy", [_POINTER], None)
_graph_options_init = _declare(
    "archbird_graph_options_init", [ctypes.POINTER(_GraphOptions)], None
)


def _bytes(value: object, label: str = "input") -> bytes:
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise TypeError(f"{label} must be bytes-like")
    return bytes(value)


def _text(value: object, label: str) -> bytes:
    if not isinstance(value, str):
        raise TypeError(f"{label} must be a string")
    return value.encode("utf-8")


def _mode(value: str) -> int:
    try:
        return {"primary": 0, "augment": 1, "audit": 2}[value]
    except (KeyError, TypeError) as error:
        raise ValueError("provider mode must be primary, augment, or audit") from error


def _raise(engine: _POINTER, status: int) -> None:
    if status == _OK:
        return
    raw = _engine_error(engine) if engine else None
    message = raw.decode("utf-8", "replace") if raw else "native operation failed"
    offset = _engine_error_offset(engine) if engine else _SIZE_MAX
    if offset == _SIZE_MAX:
        raise Error(f"{message} (status={status})", status=status)
    raise Error(
        f"{message} (status={status}, byte={offset})",
        status=status,
        offset=offset,
    )


def _new_engine(input_budget: int = 0, *, saved_artifact: bool = False) -> _POINTER:
    engine = _POINTER()
    options = _EngineOptions()
    _raise(
        engine,
        _engine_options_init_for_input(
            ctypes.byref(options), int(saved_artifact), input_budget
        ),
    )
    _raise(engine, _engine_create(ctypes.byref(options), ctypes.byref(engine)))
    return engine


def _render(engine: _POINTER, call: Callable[[_WRITE], int]) -> bytes:
    chunks = []
    callback_error: list[BaseException] = []

    @_WRITE
    def write(_user, data, length):
        try:
            if length:
                chunks.append(ctypes.string_at(data, length))
            return 0
        except BaseException as error:  # callback exceptions cannot cross C
            callback_error.append(error)
            return 1

    status = call(write)
    if callback_error:
        raise callback_error[0]
    _raise(engine, status)
    return b"".join(chunks)


def _render_to(
    engine: _POINTER,
    call: Callable[[_WRITE], int],
    sink: Callable[[bytes], object],
) -> None:
    callback_error: list[BaseException] = []

    @_WRITE
    def write(_user, data, length):
        try:
            if length:
                chunk = ctypes.string_at(data, length)
                written = sink(chunk)
                if written is not None and written != length:
                    raise OSError(
                        f"output sink wrote {written} of {length} bytes"
                    )
            return 0
        except BaseException as error:  # callback exceptions cannot cross C
            callback_error.append(error)
            return 1

    status = call(write)
    if callback_error:
        raise callback_error[0]
    _raise(engine, status)


def _one_shot(
    call: Callable[[_POINTER, _WRITE], int],
    *,
    input_budget: int = 0,
    saved_artifact: bool = False,
) -> bytes:
    engine = _new_engine(input_budget, saved_artifact=saved_artifact)
    try:
        return _render(engine, lambda write: call(engine, write))
    finally:
        _engine_destroy(engine)


IMPLEMENTATION_SHA256 = (_implementation() or b"").decode("ascii")
if len(IMPLEMENTATION_SHA256) != 64:
    raise ImportError("libarchbird returned an invalid implementation identity")


class _Project:
    __slots__ = ("engine", "project")

    def __init__(self, engine: _POINTER, project: _POINTER) -> None:
        self.engine = engine
        self.project = project

    def close(self) -> None:
        project, engine = self.project, self.engine
        self.project = _POINTER()
        self.engine = _POINTER()
        if project:
            _project_destroy(project)
        if engine:
            _engine_destroy(engine)

    def __del__(self) -> None:
        self.close()


def _project_function(name: str, extra: Sequence[object] = ()):
    return _declare(name, [_POINTER, _POINTER, *extra])


_project_create = _declare(
    "archbird_project_create",
    [_POINTER, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(_POINTER)],
)
_project_add_source = _project_function(
    "archbird_project_add_source",
    [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t],
)
_project_finalize_sources = _project_function("archbird_project_finalize_sources")
_project_set_config = _project_function(
    "archbird_project_set_config", [ctypes.c_char_p, ctypes.c_size_t]
)
_project_add_provider = _project_function(
    "archbird_project_add_provider_facts",
    [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t],
)
_project_add_observations = _project_function(
    "archbird_project_add_test_symbol_observations",
    [ctypes.c_char_p, ctypes.c_size_t],
)
_project_scan_builtin = _project_function(
    "archbird_project_scan_builtin", [ctypes.c_int]
)
_project_scan_provider = _project_function(
    "archbird_project_scan_builtin_provider",
    [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int],
)
_project_scan_provider_file = _project_function(
    "archbird_project_scan_builtin_provider_file",
    [
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.c_int,
    ],
)
_project_finalize_providers = _project_function(
    "archbird_project_finalize_providers"
)
_project_source_count = _declare(
    "archbird_project_source_count", [_POINTER], ctypes.c_size_t
)
_project_provider_count = _declare(
    "archbird_project_provider_count", [_POINTER], ctypes.c_size_t
)
_project_fact_count = _declare(
    "archbird_project_provider_fact_count", [_POINTER], ctypes.c_size_t
)
_project_manifest_digest = _declare(
    "archbird_project_manifest_sha256", [_POINTER], ctypes.c_char_p
)
_project_input_digest = _declare(
    "archbird_project_map_input_sha256", [_POINTER], ctypes.c_char_p
)
_project_config_digest = _declare(
    "archbird_project_config_sha256", [_POINTER], ctypes.c_char_p
)
_project_merge_summary = _declare(
    "archbird_project_merge_summary",
    [_POINTER, ctypes.POINTER(_MergeSummary)],
)


def project_create(manifest: bytes) -> _Project:
    data = _bytes(manifest, "manifest")
    engine = _new_engine()
    project = _POINTER()
    try:
        _raise(
            engine,
            _project_create(engine, data, len(data), ctypes.byref(project)),
        )
        return _Project(engine, project)
    except BaseException:
        if project:
            _project_destroy(project)
        _engine_destroy(engine)
        raise


def project_add_source(project: _Project, path: str, data: bytes) -> None:
    encoded = _text(path, "source path")
    source = _bytes(data, "source")
    _raise(
        project.engine,
        _project_add_source(
            project.engine,
            project.project,
            encoded,
            len(encoded),
            source,
            len(source),
        ),
    )


def project_finalize_sources(project: _Project) -> None:
    _raise(
        project.engine,
        _project_finalize_sources(project.engine, project.project),
    )


def project_set_config(project: _Project, config: bytes) -> None:
    data = _bytes(config, "config")
    _raise(
        project.engine,
        _project_set_config(project.engine, project.project, data, len(data)),
    )


def project_add_provider(project: _Project, mode: str, provider: bytes) -> None:
    data = _bytes(provider, "provider")
    _raise(
        project.engine,
        _project_add_provider(
            project.engine, project.project, _mode(mode), data, len(data)
        ),
    )


def project_add_test_symbol_observations(
    project: _Project, observations: bytes
) -> None:
    data = _bytes(observations, "observations")
    _raise(
        project.engine,
        _project_add_observations(
            project.engine, project.project, data, len(data)
        ),
    )


def project_scan_builtin(project: _Project, mode: str = "primary") -> None:
    _raise(
        project.engine,
        _project_scan_builtin(project.engine, project.project, _mode(mode)),
    )


def project_scan_builtin_provider(
    project: _Project, provider: str, mode: str = "primary"
) -> None:
    provider_id = _text(provider, "provider ID")
    _raise(
        project.engine,
        _project_scan_provider(
            project.engine,
            project.project,
            provider_id,
            len(provider_id),
            _mode(mode),
        ),
    )


def project_scan_builtin_provider_file(
    project: _Project, provider: str, path: str, mode: str = "primary"
) -> None:
    provider_id = _text(provider, "provider ID")
    source_path = _text(path, "source path")
    _raise(
        project.engine,
        _project_scan_provider_file(
            project.engine,
            project.project,
            provider_id,
            len(provider_id),
            source_path,
            len(source_path),
            _mode(mode),
        ),
    )


def project_finalize_providers(project: _Project) -> None:
    _raise(
        project.engine,
        _project_finalize_providers(project.engine, project.project),
    )


def _digest(function, project: _Project, label: str) -> str:
    value = function(project.project)
    if not value:
        raise Error(f"project has no {label} digest")
    return value.decode("ascii")


def project_manifest_sha256(project: _Project) -> str:
    return _digest(_project_manifest_digest, project, "manifest")


def project_map_input_sha256(project: _Project) -> str:
    return _digest(_project_input_digest, project, "Map-input")


def project_config_sha256(project: _Project) -> str:
    return _digest(_project_config_digest, project, "configuration")


def project_counts(project: _Project) -> dict[str, int]:
    return {
        "sources": _project_source_count(project.project),
        "providers": _project_provider_count(project.project),
        "facts": _project_fact_count(project.project),
    }


def project_merge_summary(project: _Project) -> dict[str, int]:
    summary = _MergeSummary()
    summary.struct_size = ctypes.sizeof(summary)
    _raise(
        project.engine,
        _project_merge_summary(project.project, ctypes.byref(summary)),
    )
    return {
        name: int(getattr(summary, name))
        for name, _kind in summary._fields_
        if name != "struct_size"
    }


def _project_render(name: str, project: _Project, pretty: bool = False) -> bytes:
    function = _declare(
        name, [_POINTER, _POINTER, ctypes.c_uint32, _WRITE, _POINTER]
    )
    flags = _JSON_PRETTY if pretty else 0
    return _render(
        project.engine,
        lambda write: function(
            project.engine, project.project, flags, write, None
        ),
    )


def project_file_facts(project: _Project, pretty: bool = False) -> bytes:
    return _project_render("archbird_project_render_file_facts", project, pretty)


def project_merge_ledger(project: _Project, pretty: bool = False) -> bytes:
    return _project_render("archbird_project_render_merge_ledger", project, pretty)


def project_merge_conflicts(project: _Project, pretty: bool = False) -> bytes:
    return _project_render(
        "archbird_project_render_merge_conflicts", project, pretty
    )


def project_map(project: _Project, pretty: bool = False) -> bytes:
    return _project_render("archbird_project_render_map", project, pretty)


def project_write_map(
    project: _Project, sink: Callable[[bytes], object], pretty: bool = False
) -> None:
    function = _declare(
        "archbird_project_render_map",
        [_POINTER, _POINTER, ctypes.c_uint32, _WRITE, _POINTER],
    )
    _render_to(
        project.engine,
        lambda write: function(
            project.engine,
            project.project,
            _JSON_PRETTY if pretty else 0,
            write,
            None,
        ),
        sink,
    )


def project_provider_facts(
    project: _Project, index: int, pretty: bool = False
) -> bytes:
    if index < 0:
        raise IndexError("provider index cannot be negative")
    function = _declare(
        "archbird_project_render_provider_facts",
        [_POINTER, _POINTER, ctypes.c_size_t, ctypes.c_uint32, _WRITE, _POINTER],
    )
    return _render(
        project.engine,
        lambda write: function(
            project.engine,
            project.project,
            index,
            _JSON_PRETTY if pretty else 0,
            write,
            None,
        ),
    )


def _json_flags(pretty: bool = False, trailing_newline: bool = False) -> int:
    return (_JSON_PRETTY if pretty else 0) | (
        _JSON_TRAILING_NEWLINE if trailing_newline else 0
    )


def _simple_render(
    name: str,
    values: Sequence[bytes],
    *,
    suffix_types: Sequence[object] = (),
    suffix_values: Sequence[object] = (),
    flags: int = 0,
    include_flags: bool = True,
    nullable_empty_indices: Sequence[int] = (),
    saved_artifact: bool = False,
) -> bytes:
    arguments = []
    types = [_POINTER]
    nullable = frozenset(nullable_empty_indices)
    for index, value in enumerate(values):
        arguments.extend((None if index in nullable and not value else value, len(value)))
        types.extend((ctypes.c_char_p, ctypes.c_size_t))
    types.extend(suffix_types)
    if include_flags:
        types.append(ctypes.c_uint32)
    types.extend((_WRITE, _POINTER))
    function = _declare(name, types)
    return _one_shot(
        lambda engine, write: function(
            engine, *arguments, *suffix_values, *(
                (flags, write, None) if include_flags else (write, None)
            )
        ),
        input_budget=max((len(value) for value in values), default=0),
        saved_artifact=saved_artifact,
    )


def json_canonicalize(
    input: bytes, pretty: bool = False, trailing_newline: bool = False
) -> bytes:
    return _simple_render(
        "archbird_json_canonicalize",
        [_bytes(input)],
        flags=_json_flags(pretty, trailing_newline),
    )


def project_configuration_compile(
    config: bytes, *, pretty: bool = False
) -> bytes:
    """Validate and normalize one schema-2 project configuration."""

    return _simple_render(
        "archbird_project_configuration_compile",
        [_bytes(config, "project configuration")],
        flags=_json_flags(pretty),
    )


def projection_evaluate(
    map_json: bytes,
    projection_json: bytes,
    *,
    resolution_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    """Evaluate an exhaustive typed projection over one canonical Map."""

    return _simple_render(
        "archbird_projection_evaluate",
        [
            _bytes(map_json, "Map"),
            _bytes(resolution_json),
            _bytes(projection_json, "projection definition"),
        ],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def query_plan_compile(
    config: bytes,
    map_json: bytes,
    query_id: str,
    *,
    resolution_json: bytes = b"",
    overrides_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    """Compile and evaluate one named QueryPlan."""

    return _simple_render(
        "archbird_query_plan_compile",
        [
            _bytes(config, "project configuration"),
            _bytes(map_json, "Map"),
            _bytes(resolution_json),
            _bytes(query_id.encode("utf-8"), "query id"),
            _bytes(overrides_json),
        ],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def constraints_evaluate(
    config: bytes,
    map_json: bytes,
    *,
    resolution_json: bytes = b"",
    request_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    """Evaluate schema-2 project constraints directly over one Map."""

    return _simple_render(
        "archbird_constraints_evaluate",
        [
            _bytes(config, "project configuration"),
            _bytes(map_json, "Map"),
            _bytes(resolution_json),
            _bytes(request_json),
        ],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def constraints_report(
    config: bytes,
    map_json: bytes,
    format: str,
    *,
    resolution_json: bytes = b"",
    request_json: bytes = b"",
    max_findings: int = 200,
    pretty: bool = False,
) -> bytes:
    """Evaluate project constraints and render Markdown, SARIF, or JUnit."""

    try:
        native_format = {"markdown": 1, "sarif": 2, "junit": 3}[format]
    except KeyError as error:
        raise ValueError(
            "constraint report format must be markdown, sarif, or junit"
        ) from error
    return _simple_render(
        "archbird_constraints_report",
        [
            _bytes(config, "project configuration"),
            _bytes(map_json, "Map"),
            _bytes(resolution_json),
            _bytes(request_json),
        ],
        suffix_types=(ctypes.c_int, ctypes.c_size_t),
        suffix_values=(
            native_format,
            _SIZE_MAX if max_findings < 0 else max_findings,
        ),
        flags=_json_flags(pretty, native_format == 2),
        saved_artifact=True,
    )


def constraints_freeze(
    config: bytes,
    map_json: bytes,
    *,
    owner: str,
    rationale: str,
    resolution_json: bytes = b"",
    request_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    """Freeze the complete project constraint policy as a reviewed baseline."""

    config_document = _bytes(config, "project configuration")
    map_document = _bytes(map_json, "Map")
    resolution_document = _bytes(resolution_json)
    request_document = _bytes(request_json)
    owner_data = _text(owner, "baseline owner")
    rationale_data = _text(rationale, "baseline rationale")
    function = _declare(
        "archbird_constraints_freeze",
        [
            _POINTER,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_uint32,
            _WRITE,
            _POINTER,
        ],
    )
    return _one_shot(
        lambda engine, write: function(
            engine,
            config_document,
            len(config_document),
            map_document,
            len(map_document),
            resolution_document or None,
            len(resolution_document),
            request_document or None,
            len(request_document),
            owner_data,
            len(owner_data),
            rationale_data,
            len(rationale_data),
            _json_flags(pretty, True),
            write,
            None,
        ),
        input_budget=max(
            len(config_document), len(map_document), len(resolution_document),
            len(request_document)
        ),
        saved_artifact=True,
    )


def test_symbol_observations_validate(input: bytes) -> None:
    data = _bytes(input)
    function = _declare(
        "archbird_test_symbol_observations_validate",
        [_POINTER, ctypes.c_char_p, ctypes.c_size_t],
    )
    _one_shot(
        lambda engine, _write: function(engine, data, len(data)),
        input_budget=len(data),
        saved_artifact=True,
    )


def discovery_plan(config: bytes, paths: Sequence[str], pretty=False) -> bytes:
    data = _bytes(config, "config")
    create = _declare(
        "archbird_discovery_create",
        [_POINTER, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(_POINTER)],
    )
    add = _declare(
        "archbird_discovery_add_path",
        [_POINTER, _POINTER, ctypes.c_char_p, ctypes.c_size_t],
    )
    render = _declare(
        "archbird_discovery_render",
        [_POINTER, _POINTER, ctypes.c_uint32, _WRITE, _POINTER],
    )
    engine = _new_engine(len(data))
    discovery = _POINTER()
    try:
        _raise(engine, create(engine, data, len(data), ctypes.byref(discovery)))
        for path in paths:
            encoded = _text(path, "discovery path")
            _raise(engine, add(engine, discovery, encoded, len(encoded)))
        return _render(
            engine,
            lambda write: render(
                engine,
                discovery,
                _JSON_PRETTY if pretty else 0,
                write,
                None,
            ),
        )
    finally:
        if discovery:
            _discovery_destroy(discovery)
        _engine_destroy(engine)


def discovery_resolve(
    config: bytes, request: bytes, inventory: bytes, pretty: bool = False
) -> bytes:
    return _simple_render(
        "archbird_discovery_resolve",
        [_bytes(config), _bytes(request), _bytes(inventory)],
        flags=_json_flags(pretty),
    )


def discovery_descend(
    config: bytes,
    paths: Sequence[str],
    ignore_paths: Optional[Sequence[str]] = None,
    ignore_contents: Optional[Sequence[bytes]] = None,
) -> list[bool]:
    if (ignore_paths is None) != (ignore_contents is None):
        raise TypeError("ignore paths and contents must be supplied together")
    if ignore_paths is not None and len(ignore_paths) != len(ignore_contents or ()):
        raise TypeError("ignore paths and contents must have equal lengths")
    data = _bytes(config, "config")
    create = _declare(
        "archbird_discovery_create",
        [_POINTER, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(_POINTER)],
    )
    add_ignore = _declare(
        "archbird_discovery_add_ignore",
        [
            _POINTER,
            _POINTER,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ],
    )
    descend = _declare(
        "archbird_discovery_should_descend",
        [
            _POINTER,
            _POINTER,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_int),
        ],
    )
    engine = _new_engine(len(data))
    discovery = _POINTER()
    try:
        _raise(engine, create(engine, data, len(data), ctypes.byref(discovery)))
        for path, content in zip(ignore_paths or (), ignore_contents or ()):
            encoded = _text(path, "ignore path")
            body = _bytes(content, "ignore contents")
            _raise(
                engine,
                add_ignore(
                    engine,
                    discovery,
                    encoded,
                    len(encoded),
                    body,
                    len(body),
                ),
            )
        decisions = []
        for path in paths:
            encoded = _text(path, "discovery path")
            decision = ctypes.c_int()
            _raise(
                engine,
                descend(
                    engine,
                    discovery,
                    encoded,
                    len(encoded),
                    ctypes.byref(decision),
                ),
            )
            decisions.append(bool(decision.value))
        return decisions
    finally:
        if discovery:
            _discovery_destroy(discovery)
        _engine_destroy(engine)


def map_query(map: bytes, query: bytes, pretty: bool = False) -> bytes:
    return _simple_render(
        "archbird_map_query",
        [_bytes(map), _bytes(query)],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def map_markdown(map: bytes, full=False, max_chars=0) -> bytes:
    if max_chars < 0:
        raise ValueError("map max_chars must be a nonnegative integer")
    return _simple_render(
        "archbird_map_render_markdown",
        [_bytes(map)],
        suffix_types=(ctypes.c_int, ctypes.c_size_t),
        suffix_values=(int(bool(full)), max_chars),
        include_flags=False,
        saved_artifact=True,
    )


def map_markdown_view(map: bytes, view: int, detail: int, max_chars=0) -> bytes:
    if max_chars < 0:
        raise ValueError("map max_chars must be a nonnegative integer")
    return _simple_render(
        "archbird_map_render_markdown_view",
        [_bytes(map)],
        suffix_types=(ctypes.c_int, ctypes.c_int, ctypes.c_size_t),
        suffix_values=(view, detail, max_chars),
        include_flags=False,
        saved_artifact=True,
    )


def map_query_markdown(map: bytes, query: bytes, max_chars=0) -> bytes:
    if max_chars < 0:
        raise ValueError("query max_chars must be a nonnegative integer")
    return _simple_render(
        "archbird_map_query_markdown",
        [_bytes(map), _bytes(query)],
        suffix_types=(ctypes.c_size_t,),
        suffix_values=(max_chars,),
        include_flags=False,
        saved_artifact=True,
    )


def map_query_markdown_view(
    map: bytes,
    query: bytes,
    view: int,
    detail: int,
    max_chars=0,
    verification: bytes = b"",
) -> bytes:
    if max_chars < 0:
        raise ValueError("query max_chars must be a nonnegative integer")
    return _simple_render(
        "archbird_map_query_markdown_view_with_verification",
        [_bytes(map), _bytes(query), _bytes(verification)],
        suffix_types=(ctypes.c_int, ctypes.c_int, ctypes.c_size_t),
        suffix_values=(view, detail, max_chars),
        include_flags=False,
        nullable_empty_indices=(2,),
        saved_artifact=True,
    )


def map_diff(before: bytes, after: bytes, pretty=False) -> bytes:
    return _simple_render(
        "archbird_map_diff",
        [_bytes(before), _bytes(after)],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def map_freshness(snapshot: bytes, current: bytes, pretty=False) -> bytes:
    return _simple_render(
        "archbird_map_freshness",
        [_bytes(snapshot), _bytes(current)],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def map_export_graph(
    map: bytes,
    format: str,
    view: str,
    direction="LR",
    max_nodes=200,
    max_edge_names=3,
) -> bytes:
    if max_nodes < 0 or max_edge_names < 0:
        raise ValueError("graph limits must be nonnegative integers")
    try:
        native_format = {"graphml": 0, "mermaid": 1, "json": 2}[format]
    except KeyError as error:
        raise ValueError("graph format must be graphml, json, or mermaid") from error
    try:
        native_view = {"components": 0, "files": 1, "symbols": 2}[view]
    except KeyError as error:
        raise ValueError("graph view must be components, files, or symbols") from error
    try:
        native_direction = {"LR": 0, "RL": 1, "TB": 2, "BT": 3}[direction]
    except KeyError as error:
        raise ValueError("graph direction must be BT, LR, RL, or TB") from error
    options = _GraphOptions()
    _graph_options_init(ctypes.byref(options))
    options.format = native_format
    options.view = native_view
    options.direction = native_direction
    options.max_nodes = max_nodes
    options.max_edge_names = max_edge_names
    artifact = _bytes(map)
    function = _declare(
        "archbird_map_export_graph",
        [
            _POINTER,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(_GraphOptions),
            _WRITE,
            _POINTER,
        ],
    )
    return _one_shot(
        lambda engine, write: function(
            engine, artifact, len(artifact), ctypes.byref(options), write, None
        ),
        input_budget=len(artifact),
        saved_artifact=True,
    )


def okf_analyze(
    source_bundle: bytes,
    query: bytes = b"",
    format="json",
    include_body=False,
    pretty=True,
) -> bytes:
    try:
        native_format = {"json": 0, "markdown": 1}[format]
    except KeyError as error:
        raise ValueError("OKF format must be json or markdown") from error
    source = _bytes(source_bundle)
    query_data = _bytes(query)
    function = _declare(
        "archbird_okf_analyze",
        [
            _POINTER,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_uint32,
            _WRITE,
            _POINTER,
        ],
    )
    return _one_shot(
        lambda engine, write: function(
            engine,
            source,
            len(source),
            query_data or None,
            len(query_data),
            native_format,
            int(bool(include_body)),
            _json_flags(pretty, True),
            write,
            None,
        )
    )


def okf_publish(
    map: bytes,
    verification: bytes,
    proposal: bytes,
    contract: bytes,
    result: bytes,
    normalization: bytes,
    pretty=False,
) -> bytes:
    values = [
        _bytes(value)
        for value in (map, verification, proposal, contract, result, normalization)
    ]
    return _simple_render(
        "archbird_okf_publish",
        values,
        flags=_json_flags(pretty, True),
        nullable_empty_indices=(1, 2, 3, 4, 5),
        saved_artifact=True,
    )


def workspace_plan(config: bytes, pretty=False) -> bytes:
    return _simple_render(
        "archbird_workspace_plan", [_bytes(config)], flags=_json_flags(pretty)
    )


def workspace_analyze(config: bytes, maps: bytes, pretty=False) -> bytes:
    return _simple_render(
        "archbird_workspace_analyze",
        [_bytes(config), _bytes(maps)],
        flags=_json_flags(pretty),
        saved_artifact=True,
    )


def change_proposal(
    verification: bytes,
    fingerprint: str,
    format="json",
    full=False,
    max_candidates=100,
    pretty=False,
) -> bytes:
    if max_candidates < 0:
        raise ValueError("max_candidates must be nonnegative")
    fingerprint_data = _text(fingerprint, "finding fingerprint")
    document = _bytes(verification)
    if format == "json":
        function = _declare(
            "archbird_change_proposal",
            [
                _POINTER,
                ctypes.c_char_p,
                ctypes.c_size_t,
                ctypes.c_char_p,
                ctypes.c_size_t,
                ctypes.c_uint32,
                _WRITE,
                _POINTER,
            ],
        )
        return _one_shot(
            lambda engine, write: function(
                engine,
                document,
                len(document),
                fingerprint_data,
                len(fingerprint_data),
                _json_flags(pretty),
                write,
                None,
            ),
            input_budget=len(document),
            saved_artifact=True,
        )
    if format != "markdown":
        raise ValueError("change proposal format must be json or markdown")
    function = _declare(
        "archbird_change_proposal_report",
        [
            _POINTER,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_size_t,
            _WRITE,
            _POINTER,
        ],
    )
    return _one_shot(
        lambda engine, write: function(
            engine,
            document,
            len(document),
            fingerprint_data,
            len(fingerprint_data),
            int(bool(full)),
            max_candidates,
            write,
            None,
        ),
        input_budget=len(document),
        saved_artifact=True,
    )


def change_contract(proposal: bytes, review: bytes, format="json", pretty=False):
    if format == "json":
        return _simple_render(
            "archbird_change_contract",
            [_bytes(proposal), _bytes(review)],
            flags=_json_flags(pretty),
            saved_artifact=True,
        )
    if format == "markdown":
        return _simple_render(
            "archbird_change_contract_report",
            [_bytes(proposal), _bytes(review)],
            include_flags=False,
            saved_artifact=True,
        )
    raise ValueError("change contract format must be json or markdown")


def change_verify(
    proposal: bytes,
    contract: bytes,
    before: bytes,
    after: bytes,
    format="json",
    pretty=False,
):
    values = [_bytes(value) for value in (proposal, contract, before, after)]
    if format == "json":
        return _simple_render(
            "archbird_change_verify",
            values,
            flags=_json_flags(pretty),
            saved_artifact=True,
        )
    try:
        native_format = {"markdown": 1, "sarif": 2, "junit": 3}[format]
    except KeyError as error:
        raise ValueError(
            "change result format must be json, markdown, sarif, or junit"
        ) from error
    return _simple_render(
        "archbird_change_verify_report",
        values,
        suffix_types=(ctypes.c_int,),
        suffix_values=(native_format,),
        flags=_json_flags(pretty, native_format == 2),
        saved_artifact=True,
    )
