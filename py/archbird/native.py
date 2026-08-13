"""Python host for the language-neutral native Archbird core.

The host owns source discovery/classification and CPython AST execution.  Exact
source bytes and normalized provider facts cross into the I/O-free C kernel.
"""

from __future__ import annotations

from dataclasses import dataclass
import html
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tempfile
import unicodedata
from typing import Callable, Iterable, Mapping, Optional, Sequence, Tuple, Union

from . import _native
from ._process_supervisor import supervised_ordered_map
from .errors import ConfigError
from .provider_cache import MapCacheStats, ProviderCache, ProviderCacheStats
from .providers import (
    python_ast_implementation_sha256,
    python_ast_provider_facts,
)


PATTERN_CONTRACT_VERSION = _native.PATTERN_CONTRACT_VERSION
CORE_IMPLEMENTATION_SHA256 = _native.IMPLEMENTATION_SHA256
PATTERN_CONTRACT = _native.PATTERN_CONTRACT
PATTERN_ENGINE = _native.PATTERN_ENGINE
PATTERN_UNICODE = _native.PATTERN_UNICODE
PATTERN_OPTIONS = _native.PATTERN_OPTIONS
DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS = 300.0
_DISCOVERY_MANIFEST_MAX_BYTES = 256 * 1024
_DISCOVERY_MANIFEST_LIMITS = {
    "cmake": 1,
    "npm": 128,
    "python": 32,
    "setup-cfg": 1,
}
_DISCOVERY_MANIFEST_SOURCES = {
    "cmake": {"cmake-root-project"},
    "npm": {"npm-workspace"},
    "python": {"python-top-level", "python-workspace"},
    "setup-cfg": {"python-root-setup-cfg"},
}


def validate_test_symbol_observations(observations_json: bytes) -> None:
    """Validate one canonical project-owned test-to-symbol artifact."""

    _native.test_symbol_observations_validate(observations_json)


def compile_test_observations(
    map_json: bytes,
    request_json: bytes,
    *,
    request_directory: Union[str, Path],
    repository: Union[str, Path],
) -> bytes:
    """Convert project-owned coverage reports to strict observed test routes."""

    from .adapters.coverage import compile_test_observations as compile_adapter

    encoded = compile_adapter(
        map_json,
        request_json,
        request_directory=Path(request_directory),
        repository=Path(repository),
    )
    validate_test_symbol_observations(encoded)
    return encoded


_NATIVE_CACHE_PROVIDERS = (
    (
        "lexical:c",
        lambda source: source.language in {"c", "cpp"},
        "support",
    ),
    (
        "lexical:javascript",
        lambda source: source.language
        in {"javascript", "typescript", "vue"},
        "support",
    ),
    (
        "lexical:python",
        lambda source: source.language == "python",
        "support",
    ),
    (
        "lexical:r",
        lambda source: source.language == "r",
        "support",
    ),
    (
        "syntax:tree-sitter:c",
        lambda source: source.language == "c",
        "primary",
    ),
    (
        "syntax:tree-sitter:cpp",
        lambda source: source.language == "cpp",
        "primary",
    ),
    (
        "syntax:tree-sitter:python",
        lambda source: source.language == "python",
        "support",
    ),
    (
        "syntax:tree-sitter:javascript",
        lambda source: source.language == "javascript",
        "primary",
    ),
    (
        "syntax:tree-sitter:typescript",
        lambda source: source.language == "typescript"
        and not source.path.endswith(".tsx"),
        "primary",
    ),
    (
        "syntax:tree-sitter:tsx",
        lambda source: source.language == "typescript"
        and source.path.endswith(".tsx"),
        "primary",
    ),
    (
        "syntax:tree-sitter:r",
        lambda source: source.language == "r",
        "primary",
    ),
)


def _python_provider_task(task: Tuple[str, str, bytes]) -> bytes:
    project, path, data = task
    return python_ast_provider_facts(
        project=project,
        path=path,
        source_bytes=data,
    )


def _python_provider_chunk(
    tasks: Sequence[Tuple[str, str, bytes]],
) -> Tuple[bytes, ...]:
    return tuple(_python_provider_task(task) for task in tasks)


def _parallel_python_providers(
    tasks: Sequence[Tuple[str, str, bytes]],
    workers: int,
    timeout_seconds: float,
) -> Iterable[bytes]:
    """Yield supervised provider bundles in input order."""

    chunk_size = min(64, max(1, len(tasks) // (workers * 4)))
    chunk_count = (len(tasks) + chunk_size - 1) // chunk_size
    chunks = (
        tasks[start : start + chunk_size]
        for start in range(0, len(tasks), chunk_size)
    )
    results = supervised_ordered_map(
        _python_provider_chunk,
        chunks,
        workers=min(workers, chunk_count),
        timeout_seconds=timeout_seconds,
        describe=lambda chunk: (
            chunk[0][1]
            if len(chunk) == 1
            else f"{chunk[0][1]} through {chunk[-1][1]}"
        ),
    )
    for bundles in results:
        yield from bundles


def _implementation_sha256() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def _canonical(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")


def _project_configuration_for_resolution(
    config_json: bytes, effective_config: Mapping[str, object]
) -> bytes:
    authored = json.loads(config_json) if config_json else {}
    result = dict(effective_config)
    for field in ("projections", "queries", "constraints"):
        if field in authored:
            result[field] = authored[field]
    return _canonical(result)


def _native_cache_namespace() -> str:
    return hashlib.sha256(
        b"archbird-native-provider-cache-v1\0"
        + CORE_IMPLEMENTATION_SHA256.encode("ascii")
    ).hexdigest()


def _native_provider_cache_namespace(
    classifications: Sequence[Mapping[str, object]],
) -> str:
    """Bind reusable native facts to the source-classification context."""

    context = hashlib.sha256()

    def framed(value: bytes) -> None:
        context.update(len(value).to_bytes(8, "big"))
        context.update(value)

    framed(b"archbird-native-provider-classification-v1")
    context.update(len(classifications).to_bytes(8, "big"))
    for classification in classifications:
        for field in ("path", "language", "layer"):
            framed(str(classification[field]).encode("utf-8"))
        roles = tuple(str(role) for role in classification["roles"])
        context.update(len(roles).to_bytes(8, "big"))
        for role in roles:
            framed(role.encode("utf-8"))

    identity = hashlib.sha256()
    for value in (
        b"archbird-native-provider-cache-context-v1",
        _native_cache_namespace().encode("ascii"),
        context.hexdigest().encode("ascii"),
    ):
        identity.update(len(value).to_bytes(8, "big"))
        identity.update(value)
    return identity.hexdigest()


def _python_ast_cache_namespace() -> str:
    identity = (
        f"archbird-python-ast-cache-v1\0"
        f"{python_ast_implementation_sha256()}\0"
        f"cpython-{sys.version_info.major}.{sys.version_info.minor}."
        f"{sys.version_info.micro}"
    )
    return hashlib.sha256(identity.encode("ascii")).hexdigest()


def _map_cache_namespace(mode: str) -> str:
    identity = (
        f"archbird-python-map-result-cache-v1\0"
        f"{CORE_IMPLEMENTATION_SHA256}\0"
        f"{_native_cache_namespace()}\0"
        f"{_python_ast_cache_namespace()}\0"
        f"mode={mode}"
    )
    return hashlib.sha256(identity.encode("ascii")).hexdigest()


def _source_sha256(source: "Source") -> str:
    return hashlib.sha256(source.data).hexdigest()


@dataclass(frozen=True)
class Source:
    path: str
    data: bytes
    language: str = ""
    layer: str = ""
    roles: Tuple[str, ...] = ("source",)


class Project:
    """Owned native project with deterministic host-provided source evidence."""

    def __init__(
        self,
        project: str,
        sources: Iterable[Source],
        *,
        configuration_sha256: Optional[str] = None,
        resolution: Optional[Mapping[str, object]] = None,
    ) -> None:
        ordered = tuple(sorted(sources, key=lambda item: item.path))
        if len({item.path for item in ordered}) != len(ordered):
            raise ValueError("source paths must be unique")
        self.project = project
        self.sources = ordered
        self.root: Optional[Path] = None
        self.resolution_json: Optional[bytes] = None
        self.cache_stats: Mapping[str, int] = ProviderCacheStats().as_dict()
        self.map_cache_stats: Mapping[str, int] = MapCacheStats().as_dict()
        self._config_json: Optional[bytes] = None
        self._project_configuration_json: Optional[bytes] = None
        self._authored_config_json: Optional[bytes] = None
        self._discovery_options: Optional[dict[str, object]] = None
        self._cached_map: Optional[bytes] = None
        self._map_cache: Optional[ProviderCache] = None
        self._map_cache_parameters: Optional[dict[str, str]] = None
        self._deferred_scan: Optional[dict[str, object]] = None
        classifications = [
            {
                "language": source.language,
                "layer": source.layer,
                "path": source.path,
                "roles": sorted(set(source.roles)),
            }
            for source in ordered
        ]
        self._native_provider_cache_namespace = (
            _native_provider_cache_namespace(classifications)
        )
        manifest_files = []
        for source, classification in zip(ordered, classifications):
            row = {
                "bytes": len(source.data),
                "path": source.path,
                "roles": classification["roles"],
                "sha256": hashlib.sha256(source.data).hexdigest(),
            }
            if source.language:
                row["language"] = source.language
            if source.layer:
                row["layer"] = source.layer
            manifest_files.append(row)
        manifest = {
            "artifact": "archbird-source-manifest",
            "configuration_sha256": configuration_sha256
            or hashlib.sha256(_canonical(classifications)).hexdigest(),
            "files": manifest_files,
            "producer": {
                "implementation_sha256": _implementation_sha256(),
                "name": "archbird-python-host",
                "version": "1",
            },
            "project": project,
            "schema_version": 1,
        }
        if resolution is not None:
            manifest["resolution"] = {
                "coverage": dict(resolution["coverage"]),
                "profile": dict(resolution["profile"]),
                "sha256": str(resolution["sha256"]),
            }
        self.manifest_json = _canonical(manifest)
        self._capsule = _native.project_create(self.manifest_json)
        for source in ordered:
            _native.project_add_source(self._capsule, source.path, source.data)
        _native.project_finalize_sources(self._capsule)
        self._providers_finalized = False

    @classmethod
    def from_config(
        cls,
        config_path: Union[str, Path],
        *,
        root: Optional[Union[str, Path]] = None,
        scan: bool = True,
        jobs: int = 0,
        python_provider_timeout: float = DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS,
        cache_dir: Optional[Union[str, Path]] = None,
        cache_max_bytes: Optional[int] = None,
        map_cache: bool = True,
    ) -> "Project":
        """Discover, read, and optionally analyze one configured repository."""

        path = Path(config_path).resolve()
        try:
            config_json = path.read_bytes()
        except OSError as error:
            raise ConfigError(f"cannot read configuration: {path}: {error}") from error
        metadata = json.loads(_native.discovery_plan(config_json, []))
        repository = (
            Path(root).resolve()
            if root is not None
            else (path.parent / metadata["root"]).resolve()
        )
        if not repository.is_dir():
            raise ConfigError(f"root is not a directory: {repository}")
        resolution_json = resolve_discovery(
            repository,
            config=config_json,
            ignore=False,
        )
        resolution = json.loads(resolution_json)
        sources = _read_sources(repository, resolution)
        project = cls(
            resolution["project"],
            sources,
            configuration_sha256=resolution["configuration_sha256"],
            resolution=resolution,
        )
        project.root = repository
        project.resolution_json = resolution_json
        effective_config = _canonical(resolution["effective_config"])
        project.set_config(
            effective_config,
            project_configuration_json=_project_configuration_for_resolution(
                config_json, resolution["effective_config"]
            ),
        )
        project._authored_config_json = config_json
        project._discovery_options = {
            "project": None,
            "source": (),
            "only": (),
            "exclude": (),
            "ignore": False,
            "ignore_files": (),
            "default_excludes": True,
            "max_file_bytes": None,
            "max_index_bytes": None,
            "_transient_exclude": (),
        }
        if scan:
            project.scan(
                jobs=jobs,
                python_provider_timeout=python_provider_timeout,
                cache_dir=cache_dir,
                cache_max_bytes=cache_max_bytes,
                map_cache=map_cache,
            )
        return project

    @classmethod
    def from_repository(
        cls,
        root: Union[str, Path] = ".",
        *,
        config: Optional[Union[str, Path, bytes]] = None,
        project: Optional[str] = None,
        source: Sequence[str] = (),
        only: Sequence[str] = (),
        exclude: Sequence[str] = (),
        ignore: bool = True,
        ignore_files: Sequence[Union[str, Path]] = (),
        default_excludes: bool = True,
        max_file_bytes: Optional[int] = None,
        max_index_bytes: Optional[int] = None,
        scan: bool = True,
        jobs: int = 0,
        python_provider_timeout: float = DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS,
        cache_dir: Optional[Union[str, Path]] = None,
        cache_max_bytes: Optional[int] = None,
        map_cache: bool = True,
        _transient_exclude: Sequence[Union[str, Path]] = (),
    ) -> "Project":
        """Resolve and map one repository with optional reviewed configuration."""

        repository = Path(root).resolve()
        if not repository.is_dir():
            raise ConfigError(f"root is not a directory: {repository}")
        config_json = _config_bytes(config)
        resolution_json = resolve_discovery(
            repository,
            config=config_json,
            project=project,
            source=source,
            only=only,
            exclude=exclude,
            ignore=ignore,
            ignore_files=ignore_files,
            default_excludes=default_excludes,
            max_file_bytes=max_file_bytes,
            max_index_bytes=max_index_bytes,
            _transient_exclude=_transient_exclude,
        )
        resolution = json.loads(resolution_json)
        effective_config = _canonical(resolution["effective_config"])
        sources = _read_sources(repository, resolution)
        current = cls(
            str(resolution["project"]),
            sources,
            configuration_sha256=str(resolution["configuration_sha256"]),
            resolution=resolution,
        )
        current.root = repository
        current.resolution_json = resolution_json
        current.set_config(
            effective_config,
            project_configuration_json=_project_configuration_for_resolution(
                config_json, resolution["effective_config"]
            ),
        )
        current._authored_config_json = config_json
        current._discovery_options = {
            "project": project,
            "source": tuple(source),
            "only": tuple(only),
            "exclude": tuple(exclude),
            "ignore": ignore,
            "ignore_files": tuple(ignore_files),
            "default_excludes": default_excludes,
            "max_file_bytes": max_file_bytes,
            "max_index_bytes": max_index_bytes,
            "_transient_exclude": tuple(_transient_exclude),
        }
        if scan:
            current.scan(
                jobs=jobs,
                python_provider_timeout=python_provider_timeout,
                cache_dir=cache_dir,
                cache_max_bytes=cache_max_bytes,
                map_cache=map_cache,
            )
        return current

    def with_source_overlay(
        self,
        overlay: Mapping[str, Optional[bytes]],
        *,
        config: Optional[Union[str, Path, bytes]] = None,
        scan: bool = True,
        jobs: int = 0,
        python_provider_timeout: float = DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS,
        cache_dir: Optional[Union[str, Path]] = None,
        cache_max_bytes: Optional[int] = None,
        map_cache: bool = True,
    ) -> "Project":
        """Build the same repository model from an immutable source overlay."""

        if (
            self.root is None
            or self.resolution_json is None
            or self._discovery_options is None
        ):
            raise ConfigError(
                "source overlays require a repository-backed Project"
            )
        config_json = (
            _config_bytes(config)
            if config is not None
            else bytes(self._authored_config_json or b"")
        )
        normalized = _normalize_source_overlay(overlay)
        resolution_json = resolve_discovery(
            self.root,
            config=config_json,
            _source_overlay=normalized,
            **self._discovery_options,
        )
        resolution = json.loads(resolution_json)
        effective_config = _canonical(resolution["effective_config"])
        sources = _read_sources(self.root, resolution, overlay=normalized)
        current = Project(
            str(resolution["project"]),
            sources,
            configuration_sha256=str(resolution["configuration_sha256"]),
            resolution=resolution,
        )
        current.root = self.root
        current.resolution_json = resolution_json
        current.set_config(
            effective_config,
            project_configuration_json=_project_configuration_for_resolution(
                config_json, resolution["effective_config"]
            ),
        )
        current._authored_config_json = config_json
        current._discovery_options = dict(self._discovery_options)
        if scan:
            current.scan(
                jobs=jobs,
                python_provider_timeout=python_provider_timeout,
                cache_dir=cache_dir,
                cache_max_bytes=cache_max_bytes,
                map_cache=map_cache,
            )
        return current

    @property
    def manifest_sha256(self) -> str:
        return _native.project_manifest_sha256(self._capsule)

    @property
    def map_input_sha256(self) -> str:
        """Digest of the exact mapped source path/byte inventory."""

        return _native.project_map_input_sha256(self._capsule)

    @property
    def counts(self) -> Mapping[str, int]:
        self._materialize()
        return _native.project_counts(self._capsule)

    @property
    def config_sha256(self) -> str:
        return _native.project_config_sha256(self._capsule)

    def set_config(
        self,
        config_json: bytes,
        *,
        project_configuration_json: Optional[bytes] = None,
    ) -> None:
        if self._providers_finalized:
            raise RuntimeError("providers are already finalized")
        self._config_json = bytes(config_json)
        self._project_configuration_json = bytes(
            project_configuration_json
            if project_configuration_json is not None
            else config_json
        )
        _native.project_set_config(self._capsule, self._config_json)

    @property
    def verification_configured(self) -> bool:
        if self._project_configuration_json is None:
            return False
        constraints = json.loads(self._project_configuration_json).get("constraints")
        return isinstance(constraints, dict) and bool(constraints)

    def add_provider(self, provider_json: bytes, mode: str = "primary") -> None:
        if self._providers_finalized:
            raise RuntimeError("providers are already finalized")
        _native.project_add_provider(self._capsule, mode, provider_json)

    def add_test_symbol_observations(self, observations_json: bytes) -> None:
        """Attach strict project-runner evidence without changing static facts."""

        self._materialize()
        self._map_cache = None
        self._map_cache_parameters = None
        self._cached_map = None
        _native.project_add_test_symbol_observations(
            self._capsule, observations_json
        )

    def scan_builtin_provider_file(
        self, provider_id: str, path: str, mode: str = "primary"
    ) -> None:
        if self._providers_finalized:
            raise RuntimeError("providers are already finalized")
        _native.project_scan_builtin_provider_file(
            self._capsule, provider_id, path, mode
        )

    def _cached_builtin_provider(
        self,
        cache: ProviderCache,
        *,
        namespace: str,
        provider_id: str,
        source: Source,
        mode: str,
    ) -> None:
        source_sha256 = _source_sha256(source)
        bundle = cache.load(
            namespace=namespace,
            project=self.project,
            provider_id=provider_id,
            path=source.path,
            source_sha256=source_sha256,
        )
        if bundle is not None:
            try:
                self.add_provider(bundle, mode)
                return
            except RuntimeError:
                cache.reject(
                    namespace=namespace,
                    project=self.project,
                    provider_id=provider_id,
                    path=source.path,
                    source_sha256=source_sha256,
                )
        provider_index = self.counts["providers"]
        self.scan_builtin_provider_file(provider_id, source.path, mode)
        bundle = self.provider_facts_json(provider_index)
        cache.store(
            bundle,
            namespace=namespace,
            project=self.project,
            provider_id=provider_id,
            path=source.path,
            source_sha256=source_sha256,
        )

    def _map_cache_parameters_for(
        self, mode: str
    ) -> Optional[dict[str, str]]:
        if self._config_json is None:
            return None
        return {
            "namespace": _map_cache_namespace(mode),
            "project": self.project,
            "manifest_sha256": self.manifest_sha256,
            "config_sha256": self.config_sha256,
        }

    def _cached_map_is_current(self, data: bytes) -> bool:
        try:
            document = json.loads(data)
        except (UnicodeDecodeError, json.JSONDecodeError):
            return False
        if not isinstance(document, dict):
            return False
        tool = document.get("tool")
        evidence = document.get("evidence")
        return bool(
            document.get("artifact") == "map"
            and document.get("project") == self.project
            and isinstance(document.get("schema_version"), int)
            and isinstance(tool, dict)
            and tool.get("implementation_sha256")
            == CORE_IMPLEMENTATION_SHA256
            and isinstance(evidence, dict)
            and evidence.get("config_sha256") == self.config_sha256
            and evidence.get("input_sha256") == self.map_input_sha256
        )

    def _reset_capsule(self) -> None:
        _native.project_close(self._capsule)
        self._capsule = _native.project_create(self.manifest_json)
        for source in self.sources:
            _native.project_add_source(self._capsule, source.path, source.data)
        _native.project_finalize_sources(self._capsule)
        if self._config_json is not None:
            _native.project_set_config(self._capsule, self._config_json)
        self._providers_finalized = False

    def _materialize(self) -> None:
        """Build provider state lazily when a Map-cache hit is insufficient."""

        if self._cached_map is None:
            return
        deferred = dict(self._deferred_scan or {})
        self._cached_map = None
        self._map_cache = None
        self._map_cache_parameters = None
        self._deferred_scan = None
        self._reset_capsule()
        self.scan(map_cache=False, **deferred)

    def scan(
        self,
        mode: str = "primary",
        *,
        jobs: int = 0,
        python_provider_timeout: float = DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS,
        cache_dir: Optional[Union[str, Path]] = None,
        cache_max_bytes: Optional[int] = None,
        progress: Optional[Callable[[Mapping[str, object]], None]] = None,
        map_cache: bool = True,
    ) -> None:
        """Compose lexical, portable syntax, and CPython AST evidence."""

        if self._providers_finalized:
            raise RuntimeError("providers are already finalized")
        if jobs < 0:
            raise ValueError("jobs must be zero or positive")
        if (
            isinstance(python_provider_timeout, bool)
            or not isinstance(python_provider_timeout, (int, float))
            or not math.isfinite(python_provider_timeout)
            or python_provider_timeout <= 0
        ):
            raise ValueError("python_provider_timeout must be finite and positive")
        support_mode = "augment" if mode == "primary" else mode
        cache = (
            ProviderCache(cache_dir, max_bytes=cache_max_bytes)
            if cache_dir is not None
            else None
        )

        def report(**event: object) -> None:
            if progress is not None:
                progress(event)

        map_parameters = (
            self._map_cache_parameters_for(mode)
            if cache is not None and map_cache
            else None
        )
        if cache is not None and map_parameters is not None:
            cached_map = cache.load_map(**map_parameters)
            if cached_map is not None:
                if self._cached_map_is_current(cached_map):
                    self._cached_map = cached_map
                    self._map_cache = cache
                    self._map_cache_parameters = map_parameters
                    self._deferred_scan = {
                        "mode": mode,
                        "jobs": jobs,
                        "python_provider_timeout": python_provider_timeout,
                        "cache_dir": cache_dir,
                        "cache_max_bytes": cache_max_bytes,
                    }
                    self._providers_finalized = True
                    self.cache_stats = cache.stats.as_dict()
                    self.map_cache_stats = cache.map_stats.as_dict()
                    report(phase="cache", artifact="map", state="hit")
                    return
                cache.reject_map(**map_parameters)

        if cache is None:
            for provider_id in (
                "lexical:c",
                "lexical:javascript",
                "lexical:r",
            ):
                report(phase="providers", provider=provider_id, state="start")
                _native.project_scan_builtin_provider(
                    self._capsule, provider_id, support_mode
                )
                report(phase="providers", provider=provider_id, state="complete")
            for provider_id in (
                "syntax:tree-sitter:c",
                "syntax:tree-sitter:cpp",
                "syntax:tree-sitter:javascript",
                "syntax:tree-sitter:typescript",
                "syntax:tree-sitter:tsx",
                "syntax:tree-sitter:r",
            ):
                report(phase="providers", provider=provider_id, state="start")
                _native.project_scan_builtin_provider(
                    self._capsule, provider_id, mode
                )
                report(phase="providers", provider=provider_id, state="complete")
            report(
                phase="providers",
                provider="syntax:tree-sitter:python",
                state="start",
            )
            _native.project_scan_builtin_provider(
                self._capsule, "syntax:tree-sitter:python", support_mode
            )
            report(
                phase="providers",
                provider="syntax:tree-sitter:python",
                state="complete",
            )
            report(phase="providers", provider="lexical:python", state="start")
            _native.project_scan_builtin_provider(
                self._capsule, "lexical:python", support_mode
            )
            report(
                phase="providers", provider="lexical:python", state="complete"
            )
        else:
            namespace = self._native_provider_cache_namespace
            for provider_id, matches, provider_mode in (
                _NATIVE_CACHE_PROVIDERS
            ):
                selected_mode = (
                    support_mode if provider_mode == "support" else mode
                )
                matched = tuple(source for source in self.sources if matches(source))
                report(
                    phase="providers",
                    provider=provider_id,
                    state="start",
                    total=len(matched),
                )
                for completed, source in enumerate(matched, 1):
                    self._cached_builtin_provider(
                        cache,
                        namespace=namespace,
                        provider_id=provider_id,
                        source=source,
                        mode=selected_mode,
                    )
                    report(
                        phase="providers",
                        provider=provider_id,
                        state="progress",
                        completed=completed,
                        total=len(matched),
                    )
                report(
                    phase="providers",
                    provider=provider_id,
                    state="complete",
                    completed=len(matched),
                    total=len(matched),
                )
        python_sources = tuple(
            source for source in self.sources if source.language == "python"
        )
        workers = jobs
        if workers == 0:
            workers = (
                min(8, os.cpu_count() or 1) if len(python_sources) >= 500 else 1
            )
        missing_python_sources = []
        ast_namespace = _python_ast_cache_namespace()
        if cache is not None:
            for source in python_sources:
                source_sha256 = _source_sha256(source)
                bundle = cache.load(
                    namespace=ast_namespace,
                    project=self.project,
                    provider_id="host:python-ast",
                    path=source.path,
                    source_sha256=source_sha256,
                )
                if bundle is None:
                    missing_python_sources.append(source)
                    continue
                try:
                    self.add_provider(bundle, mode)
                except RuntimeError:
                    cache.reject(
                        namespace=ast_namespace,
                        project=self.project,
                        provider_id="host:python-ast",
                        path=source.path,
                        source_sha256=source_sha256,
                    )
                    missing_python_sources.append(source)
        else:
            missing_python_sources.extend(python_sources)
        tasks = tuple(
            (self.project, source.path, source.data)
            for source in missing_python_sources
        )
        bundles: Iterable[bytes]
        report(
            phase="providers",
            provider="host:python-ast",
            state="start",
            total=len(python_sources),
        )
        if not tasks:
            bundles = ()
        elif workers == 1 or len(python_sources) < 2:
            bundles = map(_python_provider_task, tasks)
        else:
            bundles = _parallel_python_providers(
                tasks,
                workers,
                python_provider_timeout,
            )
        cached_python = len(python_sources) - len(missing_python_sources)
        for completed, (source, bundle) in enumerate(
            zip(missing_python_sources, bundles), cached_python + 1
        ):
            self.add_provider(bundle, mode)
            if cache is not None:
                cache.store(
                    bundle,
                    namespace=ast_namespace,
                    project=self.project,
                    provider_id="host:python-ast",
                    path=source.path,
                    source_sha256=_source_sha256(source),
                )
            report(
                phase="providers",
                provider="host:python-ast",
                state="progress",
                completed=completed,
                total=len(python_sources),
            )
        report(
            phase="providers",
            provider="host:python-ast",
            state="complete",
            completed=len(python_sources),
            total=len(python_sources),
        )
        report(phase="providers", provider="semantic:scip", state="start")
        _native.project_scan_builtin_provider(
            self._capsule, "semantic:scip", support_mode
        )
        report(phase="providers", provider="semantic:scip", state="complete")
        report(phase="joining", state="start")
        try:
            self.finalize_providers()
        except Exception as error:
            if self._providers_finalized:
                try:
                    error.merge_conflicts_json = self.merge_conflicts_json()
                except Exception:
                    pass
            raise
        report(phase="joining", state="complete")
        self.cache_stats = (
            cache.stats.as_dict()
            if cache is not None
            else ProviderCacheStats().as_dict()
        )
        self.map_cache_stats = (
            cache.map_stats.as_dict()
            if cache is not None
            else MapCacheStats().as_dict()
        )
        self._map_cache = cache if map_parameters is not None else None
        self._map_cache_parameters = map_parameters

    def finalize_providers(self) -> None:
        if not self._providers_finalized:
            try:
                _native.project_finalize_providers(self._capsule)
            except Exception as error:
                try:
                    _native.project_merge_summary(self._capsule)
                except Exception:
                    pass
                else:
                    self._providers_finalized = True
                    try:
                        error.merge_conflicts_json = self.merge_conflicts_json()
                    except Exception:
                        pass
                raise
            else:
                self._providers_finalized = True

    def file_facts_json(self, *, pretty: bool = False) -> bytes:
        self._materialize()
        return _native.project_file_facts(self._capsule, pretty=pretty)

    def file_facts(self) -> Mapping[str, object]:
        return json.loads(self.file_facts_json())

    def merge_ledger_json(self, *, pretty: bool = False) -> bytes:
        self._materialize()
        return _native.project_merge_ledger(self._capsule, pretty=pretty)

    def merge_conflicts_json(self, *, pretty: bool = False) -> bytes:
        """Render the bounded conflict-only provider ledger."""

        self._materialize()
        return _native.project_merge_conflicts(self._capsule, pretty=pretty)

    def merge_summary(self) -> Mapping[str, int]:
        self._materialize()
        return _native.project_merge_summary(self._capsule)

    def provider_facts_json(self, index: int, *, pretty: bool = False) -> bytes:
        """Render one normalized provider bundle by deterministic index."""

        self._materialize()
        return _native.project_provider_facts(
            self._capsule, index, pretty=pretty
        )

    def provider_facts(self, index: int) -> Mapping[str, object]:
        return json.loads(self.provider_facts_json(index))

    def map_json(self, *, pretty: bool = False) -> bytes:
        if self._cached_map is not None:
            return (
                _native.json_canonicalize(
                    self._cached_map, pretty=True, saved_artifact=True
                )
                if pretty
                else self._cached_map
            )
        data = _native.project_map(self._capsule, pretty=False)
        if self._map_cache is not None and self._map_cache_parameters is not None:
            self._map_cache.store_map(data, **self._map_cache_parameters)
            self.map_cache_stats = self._map_cache.map_stats.as_dict()
            self.cache_stats = self._map_cache.stats.as_dict()
            self._cached_map = data
        return (
            _native.json_canonicalize(data, pretty=True, saved_artifact=True)
            if pretty
            else data
        )

    def write_map_json(
        self, sink: Callable[[bytes], object], *, pretty: bool = False
    ) -> None:
        """Write the canonical Map without retaining its complete byte output."""

        if self._cached_map is not None:
            data = (
                _native.json_canonicalize(
                    self._cached_map, pretty=True, saved_artifact=True
                )
                if pretty
                else self._cached_map
            )
            written = sink(data)
            if written is not None and written != len(data):
                raise OSError(
                    f"output sink wrote {written} of {len(data)} bytes"
                )
            return
        if self._map_cache is None or self._map_cache_parameters is None:
            _native.project_write_map(self._capsule, sink, pretty=pretty)
            return
        cache_writer = self._map_cache.open_map_writer(
            **self._map_cache_parameters
        )
        if pretty:
            try:
                _native.project_write_map(
                    self._capsule, cache_writer.write, pretty=False
                )
            except BaseException:
                cache_writer.abort()
                raise
            cache_writer.commit()
            self.map_cache_stats = self._map_cache.map_stats.as_dict()
            self.cache_stats = self._map_cache.stats.as_dict()
            _native.project_write_map(self._capsule, sink, pretty=True)
            return

        def write(chunk: bytes) -> object:
            cache_writer.write(chunk)
            return sink(chunk)

        try:
            _native.project_write_map(self._capsule, write, pretty=False)
        except BaseException:
            cache_writer.abort()
            raise
        cache_writer.commit()
        self.map_cache_stats = self._map_cache.map_stats.as_dict()
        self.cache_stats = self._map_cache.stats.as_dict()

    def map(self) -> Mapping[str, object]:
        return json.loads(self.map_json())

    def map_markdown(
        self,
        *,
        view: str = "overview",
        detail: str = "standard",
        compact: bool = False,
        full: bool = False,
        max_chars: int = 0,
        group_by: str = "",
        level: str = "",
        relations: Optional[Sequence[str]] = None,
        overlays: Optional[Sequence[str]] = None,
    ) -> bytes:
        """Render one exhaustive graph projection of the current Map."""

        return render_map_markdown(
            self.map_json(),
            view=view,
            detail=detail,
            compact=compact,
            full=full,
            max_chars=max_chars,
            group_by=group_by,
            level=level,
            relations=relations,
            overlays=overlays,
            resolution_json=self.resolution_json or b"",
        )

    def source_markdown(
        self,
        *,
        artifact_json: Optional[bytes] = None,
        detail: str = "standard",
        compact: bool = False,
        full: bool = False,
        max_chars: int = 0,
    ) -> bytes:
        """Render exact project-owned source for a Map or Query selection."""

        return render_source_markdown(
            self,
            artifact_json if artifact_json is not None else self.map_json(),
            detail=detail,
            compact=compact,
            full=full,
            max_chars=max_chars,
        )

    def query_json(
        self,
        *,
        focus: Sequence[str] = (),
        paths: Sequence[str] = (),
        symbols: Sequence[str] = (),
        components: Sequence[str] = (),
        packages: Sequence[str] = (),
        artifacts: Sequence[str] = (),
        search: Sequence[str] = (),
        search_limit: int = 8,
        change_set: Optional[Mapping[str, object]] = None,
        context: Optional[Mapping[str, object]] = None,
        direction: str = "both",
        depth: int = 1,
        test_depth: int = 8,
        pretty: bool = False,
    ) -> bytes:
        return query_map_json(
            self.map_json(),
            focus=focus,
            paths=paths,
            symbols=symbols,
            components=components,
            packages=packages,
            artifacts=artifacts,
            search=search,
            search_limit=search_limit,
            change_set=change_set,
            context=context,
            direction=direction,
            depth=depth,
            test_depth=test_depth,
            resolution_json=self.resolution_json or b"",
            pretty=pretty,
        )

    def query(self, **kwargs: object) -> Mapping[str, object]:
        return json.loads(self.query_json(**kwargs))

    def path_json(
        self,
        source: Mapping[str, object],
        target: Mapping[str, object],
        *,
        level: str = "file",
        relations: Optional[Sequence[str]] = None,
        direction: str = "downstream",
        max_depth: int = 8,
        max_paths: int = 8,
        producer_policy: str = "compatible",
        pretty: bool = False,
    ) -> bytes:
        return path_map_json(
            self.map_json(),
            source,
            target,
            level=level,
            relations=relations,
            direction=direction,
            max_depth=max_depth,
            max_paths=max_paths,
            producer_policy=producer_policy,
            resolution_json=self.resolution_json or b"",
            pretty=pretty,
        )

    def path(
        self,
        source: Mapping[str, object],
        target: Mapping[str, object],
        **kwargs: object,
    ) -> Mapping[str, object]:
        return json.loads(self.path_json(source, target, **kwargs))

    def path_markdown(
        self,
        source: Mapping[str, object],
        target: Mapping[str, object],
        *,
        level: str = "file",
        relations: Optional[Sequence[str]] = None,
        direction: str = "downstream",
        max_depth: int = 8,
        max_paths: int = 8,
        producer_policy: str = "compatible",
        max_chars: int = 0,
    ) -> bytes:
        artifact = self.path_json(
            source,
            target,
            level=level,
            relations=relations,
            direction=direction,
            max_depth=max_depth,
            max_paths=max_paths,
            producer_policy=producer_policy,
        )
        return render_path_markdown(artifact, max_chars=max_chars)

    def verify_json(
        self,
        *,
        constraint_ids: Sequence[str] = (),
        baseline: Optional[Mapping[str, object]] = None,
        maps: Optional[Mapping[str, Mapping[str, object]]] = None,
        observations: Optional[Mapping[str, Mapping[str, object]]] = None,
        policy_date: Optional[str] = None,
        pretty: bool = False,
    ) -> bytes:
        """Evaluate reviewed project constraints against this exact Map."""

        if self._project_configuration_json is None:
            raise RuntimeError("verification requires project configuration")
        return evaluate_constraints_json(
            self._project_configuration_json,
            self.map_json(),
            constraint_ids=constraint_ids,
            baseline=baseline,
            maps=maps,
            observations=observations,
            policy_date=policy_date,
            resolution_json=self.resolution_json or b"",
            pretty=pretty,
        )

    def plan_json(
        self,
        verification_json: bytes,
        *,
        map_json: Optional[bytes] = None,
        before_map_json: bytes = b"",
        request_json: bytes = b"",
        pretty: bool = False,
    ) -> bytes:
        """Compile one native Plan against this Project's exact source bytes."""

        return _native.plan_compile(
            self._capsule,
            self.map_json() if map_json is None else map_json,
            verification_json,
            before_map_json,
            request_json,
            pretty=pretty,
        )

    def query_markdown(
        self,
        *,
        focus: Sequence[str] = (),
        paths: Sequence[str] = (),
        symbols: Sequence[str] = (),
        components: Sequence[str] = (),
        packages: Sequence[str] = (),
        artifacts: Sequence[str] = (),
        search: Sequence[str] = (),
        search_limit: int = 8,
        change_set: Optional[Mapping[str, object]] = None,
        context: Optional[Mapping[str, object]] = None,
        direction: str = "both",
        depth: int = 1,
        test_depth: int = 8,
        view: str = "focused",
        detail: str = "standard",
        compact: bool = False,
        full: bool = False,
        max_chars: int = 0,
    ) -> bytes:
        """Render focused context or a change brief from the same Query IR."""

        return query_map_markdown(
            self.map_json(),
            focus=focus,
            paths=paths,
            symbols=symbols,
            components=components,
            packages=packages,
            artifacts=artifacts,
            search=search,
            search_limit=search_limit,
            change_set=change_set,
            context=context,
            direction=direction,
            depth=depth,
            test_depth=test_depth,
            view=view,
            detail=detail,
            compact=compact,
            full=full,
            max_chars=max_chars,
            resolution_json=self.resolution_json or b"",
        )

    def graph_view_json(
        self,
        *,
        view: str = "components",
        query: Optional[Mapping[str, object]] = None,
        max_nodes: int = 200,
        max_edge_names: int = 3,
    ) -> bytes:
        """Render a compact deterministic graph projection for interactive UIs.

        Component and file views project the current Map. The symbol view first
        creates a focused Query from ``query`` and preserves the Query's
        evidence classifications in the graph.
        """

        artifact = (
            self.query_json(**dict(query or {}))
            if view == "symbols"
            else self.map_json()
        )
        return export_graph(
            artifact,
            format="json",
            view=view,
            max_nodes=max_nodes,
            max_edge_names=max_edge_names,
        )


class Workspace:
    """Host-loaded projects joined by the I/O-free native workspace engine."""

    def __init__(
        self,
        config_json: bytes,
        projects: Iterable[Project],
        *,
        path: Optional[Path] = None,
    ) -> None:
        self.config_json = bytes(config_json)
        self.projects = tuple(projects)
        self.path = path

    @classmethod
    def from_config(
        cls,
        config_path: Union[str, Path],
        *,
        jobs: int = 0,
        python_provider_timeout: float = DEFAULT_PYTHON_PROVIDER_TIMEOUT_SECONDS,
        cache_dir: Optional[Union[str, Path]] = None,
        cache_max_bytes: Optional[int] = None,
    ) -> "Workspace":
        path = Path(config_path).resolve()
        try:
            config_json = path.read_bytes()
        except OSError as error:
            raise ConfigError(
                f"cannot read workspace configuration: {path}: {error}"
            ) from error
        plan = json.loads(_native.workspace_plan(config_json))
        projects = []
        seen_configs: set[Path] = set()
        for index, row in enumerate(plan["projects"]):
            project_config = (path.parent / row["config"]).resolve()
            if project_config in seen_configs:
                raise ConfigError(
                    f"workspace.projects[{index}].config: duplicate project config"
                )
            seen_configs.add(project_config)
            root_value = row["root"]
            project_root = (
                (path.parent / root_value).resolve()
                if root_value is not None
                else None
            )
            projects.append(
                Project.from_config(
                    project_config,
                    root=project_root,
                    jobs=jobs,
                    python_provider_timeout=python_provider_timeout,
                    cache_dir=cache_dir,
                    cache_max_bytes=cache_max_bytes,
                )
            )
        return cls(config_json, projects, path=path)

    def json(self, *, pretty: bool = False) -> bytes:
        maps_json = b"[" + b",".join(
            project.map_json() for project in self.projects
        ) + b"]"
        return _native.workspace_analyze(
            self.config_json, maps_json, pretty=pretty
        )

    def data(self) -> Mapping[str, object]:
        return json.loads(self.json())


def _query_request(
    *,
    focus: Sequence[str] = (),
    paths: Sequence[str] = (),
    symbols: Sequence[str] = (),
    components: Sequence[str] = (),
    packages: Sequence[str] = (),
    artifacts: Sequence[str] = (),
    search: Sequence[str] = (),
    search_limit: int = 8,
    change_set: Optional[Mapping[str, object]] = None,
    context: Optional[Mapping[str, object]] = None,
    plan: Optional[Mapping[str, object]] = None,
    direction: str = "both",
    producer_policy: str = "compatible",
    depth: int = 1,
    test_depth: int = 8,
) -> bytes:
    if plan is not None:
        if (
            focus
            or paths
            or symbols
            or components
            or packages
            or artifacts
            or search
            or context is not None
            or search_limit != 8
            or direction != "both"
            or depth != 1
            or test_depth != 8
        ):
            raise ValueError(
                "QueryPlan execution accepts only change_set and producer_policy; "
                "compile query operations into the plan"
            )
        request: dict[str, object] = {
            "plan": dict(plan),
            "producer_policy": producer_policy,
        }
        if change_set is not None:
            request["change_set"] = dict(change_set)
        return _canonical(request)
    request = {
        "artifacts": list(artifacts),
        "components": list(components),
        "depth": depth,
        "direction": direction,
        "focus": list(focus),
        "packages": list(packages),
        "paths": list(paths),
        "producer_policy": producer_policy,
        "search": list(search),
        "search_limit": search_limit,
        "symbols": list(symbols),
        "test_depth": test_depth,
    }
    if context is not None:
        request["context"] = dict(context)
    if change_set is not None:
        request["change_set"] = dict(change_set)
    return _canonical(request)


def query_map_json(
    map_json: bytes,
    *,
    focus: Sequence[str] = (),
    paths: Sequence[str] = (),
    symbols: Sequence[str] = (),
    components: Sequence[str] = (),
    packages: Sequence[str] = (),
    artifacts: Sequence[str] = (),
    search: Sequence[str] = (),
    search_limit: int = 8,
    change_set: Optional[Mapping[str, object]] = None,
    context: Optional[Mapping[str, object]] = None,
    plan: Optional[Mapping[str, object]] = None,
    direction: str = "both",
    producer_policy: str = "compatible",
    depth: int = 1,
    test_depth: int = 8,
    resolution_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    request = _query_request(
        focus=focus,
        paths=paths,
        symbols=symbols,
        components=components,
        context=context,
        plan=plan,
        packages=packages,
        artifacts=artifacts,
        search=search,
        search_limit=search_limit,
        change_set=change_set,
        direction=direction,
        producer_policy=producer_policy,
        depth=depth,
        test_depth=test_depth,
    )
    return _native.map_query(
        map_json, request, pretty=pretty, resolution=resolution_json
    )


def _path_request(
    source: Mapping[str, object],
    target: Mapping[str, object],
    *,
    level: str = "file",
    relations: Optional[Sequence[str]] = None,
    direction: str = "downstream",
    max_depth: int = 8,
    max_paths: int = 8,
    producer_policy: str = "compatible",
) -> bytes:
    request: dict[str, object] = {
        "artifact": "path-request",
        "direction": direction,
        "level": level,
        "max_depth": max_depth,
        "max_paths": max_paths,
        "producer_policy": producer_policy,
        "schema_version": 1,
        "source": dict(source),
        "target": dict(target),
    }
    if relations is not None:
        request["relations"] = list(relations)
    return _canonical(request)


def path_map_json(
    map_json: bytes,
    source: Mapping[str, object],
    target: Mapping[str, object],
    *,
    level: str = "file",
    relations: Optional[Sequence[str]] = None,
    direction: str = "downstream",
    max_depth: int = 8,
    max_paths: int = 8,
    producer_policy: str = "compatible",
    resolution_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    request = _path_request(
        source,
        target,
        level=level,
        relations=relations,
        direction=direction,
        max_depth=max_depth,
        max_paths=max_paths,
        producer_policy=producer_policy,
    )
    return _native.map_path(
        map_json, request, pretty=pretty, resolution=resolution_json
    )


def render_path_markdown(path_json: bytes, *, max_chars: int = 0) -> bytes:
    """Render one canonical Path artifact without evaluating its Map again."""
    return _native.path_render_markdown(path_json, max_chars=max_chars)


def path_map_markdown(
    map_json: bytes,
    source: Mapping[str, object],
    target: Mapping[str, object],
    *,
    level: str = "file",
    relations: Optional[Sequence[str]] = None,
    direction: str = "downstream",
    max_depth: int = 8,
    max_paths: int = 8,
    producer_policy: str = "compatible",
    max_chars: int = 0,
    resolution_json: bytes = b"",
) -> bytes:
    request = _path_request(
        source,
        target,
        level=level,
        relations=relations,
        direction=direction,
        max_depth=max_depth,
        max_paths=max_paths,
        producer_policy=producer_policy,
    )
    return _native.map_path_markdown(
        map_json, request, max_chars=max_chars, resolution=resolution_json
    )


def render_map_markdown(
    map_json: bytes,
    *,
    view: str = "overview",
    detail: str = "standard",
    compact: bool = False,
    full: bool = False,
    max_chars: int = 0,
    group_by: str = "",
    level: str = "",
    relations: Optional[Sequence[str]] = None,
    overlays: Optional[Sequence[str]] = None,
    resolution_json: bytes = b"",
) -> bytes:
    """Render one exhaustive graph projection of a canonical saved Map."""

    if max_chars < 0:
        raise ValueError("max_chars must be nonnegative")
    views = {
        "overview": {
            "group_by": "directory",
            "level": "file",
            "relations": ("builds", "bridges", "imports", "packages", "tests"),
            "overlays": ("diagnostics", "evidence-quality"),
        },
        "architecture": {
            "group_by": "directory",
            "level": "file",
            "relations": (
                "bridges",
                "calls",
                "declarations",
                "imports",
                "packages",
                "references",
            ),
            "overlays": ("diagnostics", "evidence-quality"),
        },
        "tests": {
            "group_by": "directory",
            "level": "file",
            "relations": ("tests",),
            "overlays": ("diagnostics", "evidence-quality"),
        },
        "evidence": {
            "group_by": "directory",
            "level": "file",
            "relations": (),
            "overlays": ("diagnostics", "evidence-quality"),
        },
    }
    details = {"compact": 0, "standard": 1, "full": 2}
    if view not in views:
        raise ValueError("view must be overview, architecture, tests, or evidence")
    if detail not in details:
        raise ValueError("detail must be compact, standard, or full")
    if compact and full:
        raise ValueError("compact and full conflict")
    if (compact or full) and detail != "standard":
        raise ValueError("detail conflicts with compact/full alias")
    selected_detail = "compact" if compact else ("full" if full else detail)
    preset = views[view]
    selected_level = level or str(preset["level"])
    selected_group = group_by or str(preset["group_by"])
    if selected_group not in {"component", "directory", "language", "layer", "none"}:
        raise ValueError(
            "group_by must be component, directory, language, layer, or none"
        )
    if selected_level not in {"component", "file", "symbol"}:
        raise ValueError("level must be component, file, or symbol")
    if selected_level == "component":
        if group_by and selected_group != "none":
            raise ValueError("component level cannot also be grouped")
        selected_group = "none"
    selected_relations = tuple(
        relations
        if relations is not None
        else (
            ("calls", "references")
            if selected_level == "symbol"
            else preset["relations"]
        )
    )
    selected_overlays = tuple(
        overlays if overlays is not None else preset["overlays"]
    )
    definition: dict[str, object] = {
        "id": f"map-{view}",
        "level": selected_level,
        "overlays": sorted(selected_overlays),
        "relations": sorted(selected_relations),
        "select": "graph",
    }
    if selected_group != "none":
        definition["group_by"] = selected_group
    return _native.projection_render_markdown(
        map_json,
        json.dumps(definition, sort_keys=True, separators=(",", ":")).encode(),
        resolution_json=resolution_json,
        detail=details[selected_detail],
        max_chars=max_chars,
    )


def query_map_markdown(
    map_json: bytes,
    *,
    focus: Sequence[str] = (),
    paths: Sequence[str] = (),
    symbols: Sequence[str] = (),
    components: Sequence[str] = (),
    packages: Sequence[str] = (),
    artifacts: Sequence[str] = (),
    search: Sequence[str] = (),
    search_limit: int = 8,
    change_set: Optional[Mapping[str, object]] = None,
    context: Optional[Mapping[str, object]] = None,
    plan: Optional[Mapping[str, object]] = None,
    direction: str = "both",
    producer_policy: str = "compatible",
    depth: int = 1,
    test_depth: int = 8,
    view: str = "focused",
    detail: str = "standard",
    compact: bool = False,
    full: bool = False,
    max_chars: int = 0,
    verification_result: bytes = b"",
    resolution_json: bytes = b"",
) -> bytes:
    """Project a canonical Query as focused context or a change brief."""

    if max_chars < 0:
        raise ValueError("max_chars must be nonnegative")
    views = {"focused": 0, "changes": 1}
    details = {"compact": 0, "standard": 1, "full": 2}
    if view not in views:
        raise ValueError("view must be focused or changes")
    if detail not in details:
        raise ValueError("detail must be compact, standard, or full")
    if compact and full:
        raise ValueError("compact and full conflict")
    if (compact or full) and detail != "standard":
        raise ValueError("detail conflicts with compact/full alias")
    selected_detail = "compact" if compact else "full" if full else detail
    if verification_result and view != "changes":
        raise ValueError("verification_result requires the changes view")
    request = _query_request(
        focus=focus,
        paths=paths,
        symbols=symbols,
        components=components,
        context=context,
        plan=plan,
        packages=packages,
        artifacts=artifacts,
        search=search,
        search_limit=search_limit,
        change_set=change_set,
        direction=direction,
        producer_policy=producer_policy,
        depth=depth,
        test_depth=test_depth,
    )
    return _native.map_query_markdown_view(
        map_json,
        request,
        views[view],
        details[selected_detail],
        max_chars=max_chars,
        verification=verification_result,
        resolution=resolution_json,
    )


def render_source_markdown(
    project: Project,
    artifact_json: bytes,
    *,
    detail: str = "standard",
    compact: bool = False,
    full: bool = False,
    max_chars: int = 0,
) -> bytes:
    """Render hash-bound source for a canonical Map or Query selection."""

    details = {"compact": 0, "standard": 1, "full": 2}
    if detail not in details:
        raise ValueError("detail must be compact, standard, or full")
    if compact and full:
        raise ValueError("compact and full conflict")
    if (compact or full) and detail != "standard":
        raise ValueError("detail conflicts with compact/full alias")
    selected_detail = "compact" if compact else "full" if full else detail
    if max_chars < 0:
        raise ValueError("max_chars must be nonnegative")
    if selected_detail == "full" and max_chars:
        raise ValueError("full source detail cannot be combined with max_chars")
    return _native.project_source_markdown(
        project._capsule,
        artifact_json,
        detail=details[selected_detail],
        max_chars=max_chars,
    )


def diff_maps_json(before: bytes, after: bytes, *, pretty: bool = False) -> bytes:
    return _native.map_diff(before, after, pretty=pretty)


def audit_map_freshness(
    snapshot_json: bytes, current_map_json: bytes, *, pretty: bool = False
) -> bytes:
    """Classify a saved Map/Query against a freshly derived current Map."""

    return _native.map_freshness(
        snapshot_json, current_map_json, pretty=pretty
    )


def export_graph(
    map_json: bytes,
    *,
    format: str,
    view: str = "components",
    direction: str = "LR",
    max_nodes: int = 200,
    max_edge_names: int = 3,
) -> bytes:
    """Project a saved Map/Query as deterministic JSON, GraphML, or Mermaid.

    Component/file views consume a Map. The symbol view consumes a focused
    Query and is available only with ``format="json"``.
    """

    return _native.map_export_graph(
        map_json,
        format,
        view,
        direction=direction,
        max_nodes=max_nodes,
        max_edge_names=max_edge_names,
    )


def analyze_okf_source(
    source_bundle_json: bytes,
    *,
    query_json: bytes = b"",
    format: str = "json",
    include_body: bool = False,
    pretty: bool = True,
) -> bytes:
    """Apply shared native OKF policy to host-decoded syntax evidence."""

    return _native.okf_analyze(
        source_bundle_json,
        query_json,
        format,
        include_body=include_body,
        pretty=pretty,
    )


def _okf_normalization(*artifacts: bytes) -> bytes:
    texts: set[str] = set()

    def collect(value: object) -> None:
        if isinstance(value, str):
            if not value.isascii():
                texts.add(value)
        elif isinstance(value, list):
            for item in value:
                collect(item)
        elif isinstance(value, dict):
            for key, item in value.items():
                collect(key)
                collect(item)

    for encoded in artifacts:
        if encoded:
            collect(json.loads(encoded))
    if not texts:
        return b""
    rows = [
        {
            "casefold": value.casefold(),
            "slug_ascii": unicodedata.normalize("NFKD", value)
            .encode("ascii", "ignore")
            .decode("ascii"),
            "text": value,
        }
        for value in sorted(texts, key=lambda item: item.encode("utf-8"))
    ]
    return json.dumps(
        {
            "artifact": "okf-text-normalization",
            "rows": rows,
            "schema_version": 1,
        },
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")


def publish_okf_bundle(
    map_json: bytes,
    *,
    verification_json: bytes = b"",
    normalization_json: Optional[bytes] = None,
    pretty: bool = False,
) -> bytes:
    """Project canonical artifacts into a native content-addressed OKF bundle."""

    artifacts = (bytes(map_json), bytes(verification_json))
    normalization = (
        _okf_normalization(*artifacts)
        if normalization_json is None
        else bytes(normalization_json)
    )
    return _native.okf_publish(
        *artifacts,
        normalization,
        pretty=pretty,
    )


def _okf_digest(value: object) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _decode_okf_output(bundle_json: bytes) -> tuple[dict[str, object], tuple[tuple[str, str], ...]]:
    document = json.loads(bundle_json)
    if not isinstance(document, dict) or document.get("artifact") != "okf-output-bundle":
        raise ConfigError("OKF output: expected artifact='okf-output-bundle'")
    if document.get("schema_version") != 1:
        raise ConfigError("OKF output: unsupported schema_version")
    rows = document.get("files")
    if not isinstance(rows, list):
        raise ConfigError("OKF output.files: expected array")
    files: list[tuple[str, str]] = []
    previous = ""
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ConfigError(f"OKF output.files[{index}]: expected object")
        path = row.get("path")
        text = row.get("text")
        digest = row.get("sha256")
        if not all(isinstance(value, str) for value in (path, text, digest)):
            raise ConfigError(f"OKF output.files[{index}]: invalid fields")
        pure = PurePosixPath(path)
        if (
            not path
            or pure.is_absolute()
            or ".." in pure.parts
            or pure.suffix != ".md"
            or path == previous
            or (previous and path.encode("utf-8") < previous.encode("utf-8"))
        ):
            raise ConfigError(f"OKF output.files[{index}]: invalid or unsorted path")
        actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
        if digest != actual:
            raise ConfigError(f"OKF output.files[{index}]: SHA-256 mismatch")
        previous = path
        files.append((path, text))
    if not any(path == "index.md" for path, _ in files):
        raise ConfigError("OKF output: missing root index.md")
    aggregate = _okf_digest(
        [(path, hashlib.sha256(text.encode("utf-8")).hexdigest()) for path, text in files]
    )
    if document.get("sha256") != aggregate:
        raise ConfigError("OKF output: aggregate SHA-256 mismatch")
    without_integrity = [
        (path, hashlib.sha256(text.encode("utf-8")).hexdigest())
        for path, text in files
        if path != "provenance/integrity.md"
    ]
    if document.get("content_sha256") != _okf_digest(without_integrity):
        raise ConfigError("OKF output: content SHA-256 mismatch")
    by_path = dict(files)
    marker = by_path.get("provenance/integrity.md")
    if marker is None:
        raise ConfigError("OKF output: missing generated integrity concept")
    entries = _integrity_entries(marker)
    expected_paths = set(by_path) - {"provenance/integrity.md"}
    if not entries or set(entries) != expected_paths:
        raise ConfigError("OKF output: integrity inventory does not match files")
    for path, expected in entries.items():
        actual = hashlib.sha256(by_path[path].encode("utf-8")).hexdigest()
        if actual != expected:
            raise ConfigError(f"OKF output: integrity SHA-256 mismatch for {path}")
    return document, tuple(files)


_OKF_INTEGRITY_PREFIX = "| <code>"
_OKF_INTEGRITY_SEPARATOR = "</code> | <code>"
_OKF_INTEGRITY_SUFFIX = "</code> |"


def _integrity_entries(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in text.splitlines():
        if not (
            line.startswith(_OKF_INTEGRITY_PREFIX)
            and line.endswith(_OKF_INTEGRITY_SUFFIX)
        ):
            continue
        payload = line[
            len(_OKF_INTEGRITY_PREFIX) : -len(_OKF_INTEGRITY_SUFFIX)
        ]
        if _OKF_INTEGRITY_SEPARATOR not in payload:
            continue
        encoded_path, digest = payload.split(_OKF_INTEGRITY_SEPARATOR, 1)
        if len(digest) != 64 or any(
            character not in "0123456789abcdef" for character in digest
        ):
            continue
        path = html.unescape(encoded_path)
        if path in result:
            raise ConfigError(f"OKF bundle: duplicate integrity path {path!r}")
        result[path] = digest
    return result


def _recognized_generated_okf(path: Path) -> bool:
    marker = path / "provenance" / "integrity.md"
    try:
        text = marker.read_text(encoding="utf-8")
    except OSError:
        return False
    if not (
        'type: "Archbird Bundle Integrity"' in text
        and '"producer":{"implementation_sha256":' in text
    ):
        return False
    entries = _integrity_entries(text)
    actual_files = {
        item.relative_to(path).as_posix()
        for item in path.rglob("*")
        if item.is_file() and item != marker
    }
    if not entries or set(entries) != actual_files:
        return False
    expected_directories = {
        parent.as_posix()
        for relative in entries
        for parent in PurePosixPath(relative).parents
        if parent.as_posix() != "."
    }
    actual_directories = {
        item.relative_to(path).as_posix()
        for item in path.rglob("*")
        if item.is_dir() and not item.is_symlink()
    }
    if actual_directories != expected_directories:
        return False
    if any(item.is_symlink() for item in path.rglob("*")):
        return False
    return all(
        hashlib.sha256((path / relative).read_bytes()).hexdigest() == digest
        for relative, digest in entries.items()
    )


def write_okf_bundle(
    bundle_json: bytes,
    output: Union[str, Path],
    *,
    replace: bool = False,
    source_paths: Sequence[Union[str, Path]] = (),
) -> None:
    """Atomically install a native OKF output bundle with safe replacement."""

    _document, files = _decode_okf_output(bundle_json)
    requested = Path(output)
    if requested.is_symlink():
        raise ConfigError(f"OKF output must not be a symlink: {requested}")
    target = requested.resolve()
    for source in source_paths:
        source_path = Path(source).resolve()
        if target == source_path or target in source_path.parents:
            raise ConfigError(
                f"OKF output would contain and replace source artifact: {source_path}"
            )
    if target.exists() and not target.is_dir():
        raise ConfigError(f"OKF output exists and is not a directory: {target}")
    existing = target.exists() and any(target.iterdir())
    if existing and not replace:
        raise ConfigError(
            "OKF output directory is not empty; pass --replace for a generated "
            f"bundle: {target}"
        )
    if existing and not _recognized_generated_okf(target):
        raise ConfigError(
            "OKF output is not a recognized Archbird-generated bundle; refusing "
            f"replacement: {target}"
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(
        tempfile.mkdtemp(prefix=f".{target.name}.archbird-okf-", dir=target.parent)
    )
    backup: Optional[Path] = None
    try:
        for relative, text in files:
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("w", encoding="utf-8", newline="\n") as stream:
                stream.write(text)
        if target.exists():
            if not any(target.iterdir()):
                target.rmdir()
            else:
                backup = Path(
                    tempfile.mkdtemp(
                        prefix=f".{target.name}.archbird-backup-", dir=target.parent
                    )
                )
                backup.rmdir()
                os.replace(target, backup)
        try:
            os.replace(temporary, target)
        except OSError as install_error:
            if backup is not None and backup.exists():
                if target.exists():
                    preserved = backup
                    backup = None
                    raise ConfigError(
                        "cannot install OKF bundle because the target reappeared; "
                        f"the prior bundle is preserved at {preserved}"
                    ) from install_error
                try:
                    os.replace(backup, target)
                    backup = None
                except OSError as restore_error:
                    preserved = backup
                    backup = None
                    raise ConfigError(
                        "cannot install or restore OKF bundle; the prior bundle is "
                        f"preserved at {preserved}: {restore_error}"
                    ) from install_error
            raise
        if backup is not None:
            shutil.rmtree(backup)
            backup = None
    except ConfigError:
        raise
    except OSError as error:
        raise ConfigError(f"cannot write OKF bundle {target}: {error}") from error
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
        if backup is not None and backup.exists():
            shutil.rmtree(backup)


def export_okf_bundle(
    map_path: Union[str, Path],
    output: Union[str, Path],
    *,
    verification_path: Optional[Union[str, Path]] = None,
    replace: bool = False,
) -> bytes:
    """Read stable canonical inputs, publish, and atomically install OKF."""

    map_source = Path(map_path).resolve()
    optional_sources = (
        Path(verification_path).resolve() if verification_path is not None else None,
    )
    paths = [map_source, *(path for path in optional_sources if path is not None)]
    before_by_path = {path: path.read_bytes() for path in paths}
    bundle = publish_okf_bundle(
        before_by_path[map_source],
        verification_json=(
            before_by_path[optional_sources[0]] if optional_sources[0] else b""
        ),
    )
    for path, expected in before_by_path.items():
        if path.read_bytes() != expected:
            raise ConfigError(f"OKF source artifact changed during export: {path}")
    write_okf_bundle(bundle, output, replace=replace, source_paths=paths)
    return bundle


def analyze_workspace_json(
    config_json: bytes,
    maps: Iterable[bytes],
    *,
    pretty: bool = False,
) -> bytes:
    maps_json = b"[" + b",".join(bytes(item) for item in maps) + b"]"
    return _native.workspace_analyze(config_json, maps_json, pretty=pretty)


def compile_project_configuration(
    config_json: bytes, *, pretty: bool = False
) -> bytes:
    """Validate and normalize one archbird.json document."""

    return _native.project_configuration_compile(config_json, pretty=pretty)


def evaluate_projection_json(
    map_json: bytes,
    projection: Mapping[str, object],
    *,
    resolution_json: bytes = b"",
    pretty: bool = False,
) -> bytes:
    """Evaluate one exhaustive typed projection over canonical Map evidence."""

    return _native.projection_evaluate(
        map_json,
        _canonical(projection),
        resolution_json=resolution_json,
        pretty=pretty,
    )


def compile_query_plan_json(
    config_json: bytes,
    query_id: str = "",
    *,
    overrides: Optional[Mapping[str, object]] = None,
    pretty: bool = False,
) -> bytes:
    """Compile one saved or ad-hoc Map-independent QueryPlan."""

    return _native.query_plan_compile(
        config_json,
        query_id,
        overrides_json=_canonical(dict(overrides or {})),
        pretty=pretty,
    )


def validate_plan(plan_json: bytes) -> None:
    """Validate one canonical Plan artifact."""

    _native.plan_validate(plan_json)


def render_plan_markdown(plan_json: bytes) -> bytes:
    """Render one canonical Plan as a concise task packet."""

    return _native.plan_render_markdown(plan_json)


def compile_plan_json(
    project: Project,
    map_json: bytes,
    verification_json: bytes,
    *,
    before_map_json: bytes = b"",
    request: Optional[Mapping[str, object]] = None,
    pretty: bool = False,
) -> bytes:
    """Compile a Plan in the native core against exact project source bytes."""

    if not isinstance(project, Project):
        raise TypeError("Plan compilation requires a Project")
    return project.plan_json(
        verification_json,
        map_json=map_json,
        before_map_json=before_map_json,
        request_json=_canonical(dict(request or {})) if request else b"",
        pretty=pretty,
    )


def validate_act(act_json: bytes) -> None:
    """Validate one canonical Act artifact."""

    _native.act_validate(act_json)


def plan_source_requirements(
    plan_json: bytes,
    executor_submissions_json: bytes = b"",
    *,
    pretty: bool = False,
) -> bytes:
    """Return the source observations required to materialize a Plan."""

    return _native.plan_source_requirements(
        plan_json, executor_submissions_json, pretty=pretty
    )


def act_source_requirements(
    act_json: bytes, *, pretty: bool = False
) -> bytes:
    """Return the source observations required to apply an Act."""

    return _native.act_source_requirements(act_json, pretty=pretty)


def materialize_act_json(
    project: Project,
    plan_json: bytes,
    map_json: bytes,
    verification_json: bytes,
    source_metadata_json: bytes,
    executor_submissions_json: bytes = b"",
    *,
    pretty: bool = False,
) -> bytes:
    """Materialize exact Plan operations into a source-locked Act."""

    if not isinstance(project, Project):
        raise TypeError("Act materialization requires a Project")
    return _native.act_materialize(
        project._capsule,
        plan_json,
        map_json,
        verification_json,
        source_metadata_json,
        executor_submissions_json,
        pretty=pretty,
    )


def accept_act_json(
    act_json: bytes,
    before_map_json: bytes,
    after_map_json: bytes,
    verification_json: bytes,
    gate_results_json: bytes = b"",
    *,
    pretty: bool = False,
) -> bytes:
    """Bind a materialized Act to its verified isolated after-state."""

    return _native.act_accept(
        act_json,
        before_map_json,
        after_map_json,
        verification_json,
        gate_results_json,
        pretty=pretty,
    )


def preflight_act_apply(
    act_json: bytes, source_metadata_json: bytes
) -> str:
    """Classify an accepted Act against current source observations."""

    state = _native.act_preflight_apply(act_json, source_metadata_json)
    if state == 0:
        return "ready"
    if state == 1:
        return "already_satisfied"
    raise RuntimeError(f"native Act preflight returned unknown state {state}")


def _constraint_request_json(
    *,
    constraint_ids: Sequence[str] = (),
    baseline: Optional[Mapping[str, object]] = None,
    maps: Optional[Mapping[str, Mapping[str, object]]] = None,
    observations: Optional[Mapping[str, Mapping[str, object]]] = None,
    policy_date: Optional[str] = None,
) -> bytes:
    request: dict[str, object] = {}
    if constraint_ids:
        request["ids"] = list(constraint_ids)
    if baseline is not None:
        request["baseline"] = dict(baseline)
    if maps is not None:
        request["maps"] = {name: dict(document) for name, document in maps.items()}
    if observations is not None:
        request["observations"] = {
            name: dict(document) for name, document in observations.items()
        }
    if policy_date is not None:
        request["policy_date"] = policy_date
    return _canonical(request) if request else b""


def evaluate_constraints_json(
    config_json: bytes,
    map_json: bytes,
    *,
    constraint_ids: Sequence[str] = (),
    baseline: Optional[Mapping[str, object]] = None,
    maps: Optional[Mapping[str, Mapping[str, object]]] = None,
    observations: Optional[Mapping[str, Mapping[str, object]]] = None,
    policy_date: Optional[str] = None,
    resolution_json: bytes = b"",
    format: str = "json",
    max_findings: int = 200,
    pretty: bool = False,
) -> bytes:
    """Evaluate all or selected project constraints."""

    request_json = _constraint_request_json(
        constraint_ids=constraint_ids,
        baseline=baseline,
        maps=maps,
        observations=observations,
        policy_date=policy_date,
    )
    if format == "json":
        return _native.constraints_evaluate(
            config_json,
            map_json,
            resolution_json=resolution_json,
            request_json=request_json,
            pretty=pretty,
        )
    return _native.constraints_report(
        config_json,
        map_json,
        format,
        resolution_json=resolution_json,
        request_json=request_json,
        max_findings=max_findings,
        pretty=pretty,
    )


def _evaluate_constraints_report_with_blocking(
    config_json: bytes,
    map_json: bytes,
    *,
    constraint_ids: Sequence[str] = (),
    baseline: Optional[Mapping[str, object]] = None,
    maps: Optional[Mapping[str, Mapping[str, object]]] = None,
    observations: Optional[Mapping[str, Mapping[str, object]]] = None,
    policy_date: Optional[str] = None,
    resolution_json: bytes = b"",
    format: str = "markdown",
    max_findings: int = 200,
    pretty: bool = False,
) -> tuple[bytes, bool]:
    """Render constraints and return their blocking state from one evaluation."""

    request_json = _constraint_request_json(
        constraint_ids=constraint_ids,
        baseline=baseline,
        maps=maps,
        observations=observations,
        policy_date=policy_date,
    )
    return _native.constraints_report_with_blocking(
        config_json,
        map_json,
        format,
        resolution_json=resolution_json,
        request_json=request_json,
        max_findings=max_findings,
        pretty=pretty,
    )


def freeze_constraints_json(
    config_json: bytes,
    map_json: bytes,
    *,
    owner: str,
    rationale: str,
    baseline: Optional[Mapping[str, object]] = None,
    maps: Optional[Mapping[str, Mapping[str, object]]] = None,
    observations: Optional[Mapping[str, Mapping[str, object]]] = None,
    policy_date: Optional[str] = None,
    resolution_json: bytes = b"",
    pretty: bool = True,
) -> bytes:
    """Freeze the complete project constraint policy as a reviewed baseline."""

    request: dict[str, object] = {}
    if baseline is not None:
        request["baseline"] = dict(baseline)
    if maps is not None:
        request["maps"] = {name: dict(document) for name, document in maps.items()}
    if observations is not None:
        request["observations"] = {
            name: dict(document) for name, document in observations.items()
        }
    if policy_date is not None:
        request["policy_date"] = policy_date
    return _native.constraints_freeze(
        config_json,
        map_json,
        owner=owner,
        rationale=rationale,
        resolution_json=resolution_json,
        request_json=_canonical(request) if request else b"",
        pretty=pretty,
    )


def _strict_document(raw: bytes, label: str) -> Mapping[str, object]:
    try:
        canonical = _native.json_canonicalize(raw)
        value = json.loads(canonical)
    except (RuntimeError, ValueError, json.JSONDecodeError) as error:
        raise ConfigError(f"invalid {label}: {error}") from error
    if not isinstance(value, dict):
        raise ConfigError(f"invalid {label}: expected object")
    return value


def _inventory_state(
    config_json: bytes,
    root: Path,
    *,
    include_standard_ignores: bool = False,
    ignore_files: Sequence[str] = (),
    collect_sizes: bool = False,
    transient_exclude: Sequence[str] = (),
) -> tuple[Tuple[str, ...], Tuple[str, ...], Tuple[Tuple[str, int], ...]]:
    files: list[str] = []
    file_sizes: list[tuple[str, int]] = []
    pruned: list[str] = []
    pending: list[tuple[str, Path]] = [("", root)]
    standard_ignores: list[tuple[str, bytes]] = []
    custom_paths = tuple(ignore_files)
    custom_set = set(custom_paths)
    transient_set = set(transient_exclude)
    custom_ignores = [
        (relative, root.joinpath(*PurePosixPath(relative).parts).read_bytes())
        for relative in custom_paths
    ]
    while pending:
        directories: list[tuple[str, Path]] = []
        for directory_relative, directory in pending:
            try:
                with os.scandir(directory) as entries:
                    ordered = sorted(entries, key=lambda entry: entry.name.encode())
                    for entry in ordered:
                        relative = (
                            f"{directory_relative}/{entry.name}"
                            if directory_relative
                            else entry.name
                        )
                        if entry.is_dir(follow_symlinks=False):
                            directories.append((relative, Path(entry.path)))
                        elif entry.is_file(follow_symlinks=False):
                            if relative in transient_set:
                                continue
                            files.append(relative)
                            if collect_sizes:
                                try:
                                    metadata = entry.stat(follow_symlinks=False)
                                except FileNotFoundError:
                                    continue
                                except OSError as error:
                                    raise ConfigError(
                                        f"cannot stat repository file: {relative}: "
                                        f"{error}"
                                    ) from error
                                file_sizes.append((relative, metadata.st_size))
                    if include_standard_ignores:
                        for name in (".gitignore", ".ignore", ".archbirdignore"):
                            relative = (
                                f"{directory_relative}/{name}"
                                if directory_relative
                                else name
                            )
                            if relative in custom_set:
                                continue
                            candidate = directory / name
                            if candidate.is_file() and not candidate.is_symlink():
                                standard_ignores.append(
                                    (relative, candidate.read_bytes())
                                )
            except OSError as error:
                raise ConfigError(
                    f"cannot enumerate repository directory: {directory}: {error}"
                ) from error
        directories.sort(key=lambda row: row[0].encode())
        active_ignores = [*standard_ignores, *custom_ignores]
        decisions = _native.discovery_descend(
            config_json,
            [relative for relative, _ in directories],
            [relative for relative, _ in active_ignores],
            [contents for _, contents in active_ignores],
        )
        pending = [
            (relative, candidate)
            for (relative, candidate), should_descend in zip(
                directories, decisions
            )
            if should_descend
        ]
        pruned.extend(
            relative
            for (relative, _), should_descend in zip(directories, decisions)
            if not should_descend
        )
    return (
        tuple(sorted(files)),
        tuple(sorted(pruned)),
        tuple(sorted(file_sizes)),
    )


def _config_bytes(config: Optional[Union[str, Path, bytes]]) -> bytes:
    if config is None:
        return b""
    if isinstance(config, bytes):
        return config
    path = Path(config).resolve()
    try:
        return path.read_bytes()
    except OSError as error:
        raise ConfigError(f"cannot read configuration: {path}: {error}") from error


def _source_rows(values: Sequence[str]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for value in values:
        language, separator, glob = value.partition("=")
        if not separator or not language or not glob:
            raise ConfigError(f"--source: expected LANGUAGE=GLOB, got {value!r}")
        rows.append({"glob": glob, "language": language})
    return rows


def _map_request(
    *,
    project: Optional[str],
    source: Sequence[str],
    only: Sequence[str],
    exclude: Sequence[str],
    ignore_files: Sequence[str],
    use_ignore_files: bool,
    default_excludes: bool,
    max_file_bytes: Optional[int],
    max_index_bytes: Optional[int],
) -> bytes:
    request: dict[str, object] = {
        "artifact": "archbird-map-request",
        "exclude": list(exclude),
        "ignore_files": list(ignore_files),
        "only": list(only),
        "schema_version": 1,
        "sources": _source_rows(source),
    }
    if not default_excludes:
        request["default_excludes"] = False
    if not use_ignore_files:
        request["ignore"] = False
    if project is not None:
        request["project"] = project
    if max_file_bytes is not None:
        if max_file_bytes <= 0:
            raise ConfigError("--max-file-bytes must be positive")
        request["max_file_bytes"] = max_file_bytes
    if max_index_bytes is not None:
        if max_index_bytes <= 0:
            raise ConfigError("--max-index-bytes must be positive")
        request["max_index_bytes"] = max_index_bytes
    return _canonical(request)


def _file_row(root: Path, path: str) -> Optional[dict[str, object]]:
    candidate = root.joinpath(*PurePosixPath(path).parts)
    try:
        metadata = candidate.lstat()
    except FileNotFoundError:
        return None
    except OSError as error:
        raise ConfigError(f"cannot stat repository file: {path}: {error}") from error
    if not stat.S_ISREG(metadata.st_mode):
        return None
    return {"bytes": metadata.st_size, "path": path}


def _inventory_rows(
    config_json: bytes,
    root: Path,
    *,
    include_standard_ignores: bool = False,
    ignore_files: Sequence[str] = (),
    transient_exclude: Sequence[str] = (),
) -> tuple[list[dict[str, object]], Tuple[str, ...]]:
    _paths, pruned, file_sizes = _inventory_state(
        config_json,
        root,
        include_standard_ignores=include_standard_ignores,
        ignore_files=ignore_files,
        collect_sizes=True,
        transient_exclude=transient_exclude,
    )
    return [
        {"bytes": size, "path": path}
        for path, size in file_sizes
    ], pruned


def _safe_relative(root: Path, value: Union[str, Path]) -> str:
    raw = Path(value)
    candidate = raw.resolve() if raw.is_absolute() else (root / raw).resolve()
    try:
        relative = candidate.relative_to(root).as_posix()
    except ValueError as error:
        raise ConfigError(f"path is outside repository root: {value}") from error
    if not relative or relative == ".":
        raise ConfigError(f"path is not a repository file: {value}")
    return relative


def _ignore_sort_key(path: str) -> tuple[int, tuple[str, ...], int, str]:
    parts = PurePosixPath(path).parts
    priority = {".gitignore": 0, ".ignore": 1, ".archbirdignore": 2}.get(
        parts[-1], 3
    )
    return (len(parts) - 1, parts[:-1], priority, path)


def _normalize_source_overlay(
    overlay: Mapping[str, Optional[bytes]],
) -> dict[str, Optional[bytes]]:
    if not isinstance(overlay, Mapping):
        raise ConfigError("source overlay must be a path-to-bytes mapping")
    normalized: dict[str, Optional[bytes]] = {}
    for path, data in overlay.items():
        if not isinstance(path, str):
            raise ConfigError("source overlay paths must be strings")
        parsed = PurePosixPath(path)
        if (
            not path
            or parsed.is_absolute()
            or parsed.as_posix() != path
            or any(part in ("", ".", "..") for part in parsed.parts)
        ):
            raise ConfigError(f"invalid source overlay path: {path!r}")
        if data is not None and not isinstance(data, bytes):
            raise ConfigError(f"source overlay value must be bytes or null: {path}")
        normalized[path] = data
    return normalized


def _overlay_changes_ignore_inputs(
    root: Path,
    overlay: Mapping[str, Optional[bytes]],
    custom_ignore_files: Sequence[str],
    *,
    include_standard_ignores: bool,
) -> bool:
    ignore_names = {".gitignore", ".ignore", ".archbirdignore"}
    protected = {
        path
        for path in overlay
        if path in custom_ignore_files
        or (
            include_standard_ignores
            and PurePosixPath(path).name in ignore_names
        )
    }
    for path in sorted(protected):
        candidate = root.joinpath(*PurePosixPath(path).parts)
        try:
            current = candidate.read_bytes()
        except FileNotFoundError:
            current = None
        except OSError as error:
            raise ConfigError(
                f"cannot read discovery ignore input: {path}: {error}"
            ) from error
        if overlay[path] != current:
            return True
    return False


def _overlay_rows(
    rows: Sequence[Mapping[str, object]],
    overlay: Mapping[str, Optional[bytes]],
) -> list[dict[str, object]]:
    indexed = {
        str(row["path"]): {"bytes": int(row["bytes"]), "path": str(row["path"])}
        for row in rows
    }
    for path, data in overlay.items():
        if data is None:
            indexed.pop(path, None)
        else:
            indexed[path] = {"bytes": len(data), "path": path}
    return [indexed[path] for path in sorted(indexed, key=lambda value: value.encode())]


def _read_discovery_input(
    root: Path, path: str, max_bytes: Optional[int]
) -> bytes:
    parts = PurePosixPath(path).parts
    cursor = root
    for part in parts[:-1]:
        cursor /= part
        metadata = cursor.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            raise OSError(f"unsafe discovery input parent: {path}")
    candidate = root.joinpath(*parts)
    before = candidate.lstat()
    if stat.S_ISLNK(before.st_mode) or not stat.S_ISREG(before.st_mode):
        raise OSError(f"discovery input is not a regular file: {path}")
    if max_bytes is not None and before.st_size > max_bytes:
        raise OSError(
            f"discovery input exceeds advertised limit: {path}: "
            f"{before.st_size} > {max_bytes}"
        )
    descriptor = os.open(
        candidate, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    )
    try:
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or opened.st_dev != before.st_dev
            or opened.st_ino != before.st_ino
        ):
            raise OSError(f"discovery input changed while opening: {path}")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            if max_bytes is not None and total > max_bytes:
                raise OSError(
                    f"discovery input exceeds advertised limit: {path}: "
                    f"{total} > {max_bytes}"
                )
        after = os.fstat(descriptor)
        if (
            after.st_dev != opened.st_dev
            or after.st_ino != opened.st_ino
            or after.st_size != opened.st_size
            or after.st_mtime_ns != opened.st_mtime_ns
            or total != opened.st_size
        ):
            raise OSError(f"discovery input changed while reading: {path}")
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _encoded_input(
    root: Path,
    path: str,
    overlay: Optional[Mapping[str, Optional[bytes]]] = None,
    *,
    max_bytes: Optional[int] = None,
) -> dict[str, str]:
    if overlay is not None and path in overlay:
        data = overlay[path]
        if data is None:
            raise ConfigError(f"discovery input was removed by source overlay: {path}")
    else:
        try:
            data = _read_discovery_input(root, path, max_bytes)
        except OSError as error:
            raise ConfigError(
                f"cannot read discovery input: {path}: {error}"
            ) from error
    assert data is not None
    if max_bytes is not None and len(data) > max_bytes:
        raise ConfigError(
            f"discovery input exceeds advertised limit: {path}: "
            f"{len(data)} > {max_bytes}"
        )
    return {"content_hex": data.hex(), "path": path}


def _repository_inventory(
    root: Path,
    rows: Sequence[Mapping[str, object]],
    *,
    include_standard_ignores: bool,
    ignore_files: Sequence[Union[str, Path]],
    pruned_directories: Sequence[str] = (),
    overlay: Optional[Mapping[str, Optional[bytes]]] = None,
    manifest_requests: Sequence[Mapping[str, object]] = (),
) -> bytes:
    paths = {str(row["path"]) for row in rows}
    standard = sorted(
        (
            path
            for path in paths
            if PurePosixPath(path).name
            in {".gitignore", ".ignore", ".archbirdignore"}
        ),
        key=_ignore_sort_key,
    )
    custom = [str(value) for value in ignore_files]
    custom_set = set(custom)
    selected = (
        [path for path in standard if path not in custom_set]
        if include_standard_ignores
        else []
    )
    seen = set(selected)
    for relative in custom:
        if relative not in seen:
            selected.append(relative)
            seen.add(relative)
    documents = []
    if "package.json" in paths:
        documents.append(_encoded_input(root, "package.json", overlay))
    if "pyproject.toml" in paths:
        documents.append(_encoded_input(root, "pyproject.toml", overlay))
    if "DESCRIPTION" in paths:
        documents.append(_encoded_input(root, "DESCRIPTION", overlay))
    if "configure.ac" in paths:
        documents.append(_encoded_input(root, "configure.ac", overlay))
    document_paths = {str(row["path"]) for row in documents}
    manifest_counts = {
        kind: 0 for kind in _DISCOVERY_MANIFEST_LIMITS
    }
    for request in manifest_requests:
        if not isinstance(request, Mapping):
            raise ConfigError("native manifest request is invalid")
        path = request.get("path")
        max_bytes = request.get("max_bytes")
        kind = request.get("kind")
        source = request.get("source")
        if (
            not isinstance(path, str)
            or path in document_paths
            or path not in paths
            or not isinstance(max_bytes, int)
            or isinstance(max_bytes, bool)
            or max_bytes != _DISCOVERY_MANIFEST_MAX_BYTES
            or request.get("fulfilled") is not False
            or not isinstance(kind, str)
            or kind not in _DISCOVERY_MANIFEST_LIMITS
            or not isinstance(source, str)
            or source not in _DISCOVERY_MANIFEST_SOURCES.get(kind, set())
        ):
            raise ConfigError("native manifest request is invalid")
        assert isinstance(kind, str)
        manifest_counts[kind] += 1
        if manifest_counts[kind] > _DISCOVERY_MANIFEST_LIMITS[kind]:
            raise ConfigError("native manifest request limit was exceeded")
        documents.append(
            _encoded_input(root, path, overlay, max_bytes=max_bytes)
        )
        document_paths.add(path)
    return _canonical(
        {
            "artifact": "archbird-repository-inventory",
            "documents": documents,
            "files": list(rows),
            "ignore_files": [
                _encoded_input(root, path, overlay) for path in selected
            ],
            "pruned_directories": list(pruned_directories),
            "schema_version": 1,
        }
    )


def _root_rows(
    root: Path, transient_exclude: Sequence[str] = ()
) -> list[dict[str, object]]:
    paths = (
        ".archbirdignore",
        ".gitignore",
        ".ignore",
        "Makefile",
        "DESCRIPTION",
        "NAMESPACE",
        "configure.ac",
        "package.json",
        "pyproject.toml",
        "setup.cfg",
        "CMakeLists.txt",
    )
    excluded = set(transient_exclude)
    return [
        row
        for path in paths
        if path not in excluded and (row := _file_row(root, path)) is not None
    ]


def resolve_discovery(
    root: Union[str, Path] = ".",
    *,
    config: Optional[Union[str, Path, bytes]] = None,
    project: Optional[str] = None,
    source: Sequence[str] = (),
    only: Sequence[str] = (),
    exclude: Sequence[str] = (),
    ignore: bool = True,
    ignore_files: Sequence[Union[str, Path]] = (),
    default_excludes: bool = True,
    max_file_bytes: Optional[int] = None,
    max_index_bytes: Optional[int] = None,
    pretty: bool = False,
    _transient_exclude: Sequence[Union[str, Path]] = (),
    _source_overlay: Optional[Mapping[str, Optional[bytes]]] = None,
) -> bytes:
    """Return the canonical C-owned config-resolution artifact for a repository."""

    repository = Path(root).resolve()
    if not repository.is_dir():
        raise ConfigError(f"root is not a directory: {repository}")
    config_json = _config_bytes(config)
    overlay = _normalize_source_overlay(_source_overlay or {})
    normalized_ignore_files = tuple(
        dict.fromkeys(_safe_relative(repository, value) for value in ignore_files)
    )
    normalized_transient_exclude = tuple(
        dict.fromkeys(
            _safe_relative(repository, value) for value in _transient_exclude
        )
    )
    ignore_inputs_changed = _overlay_changes_ignore_inputs(
        repository,
        overlay,
        normalized_ignore_files,
        include_standard_ignores=ignore,
    )
    request = _map_request(
        project=project,
        source=source,
        only=only,
        exclude=exclude,
        ignore_files=normalized_ignore_files,
        use_ignore_files=ignore,
        default_excludes=default_excludes,
        max_file_bytes=max_file_bytes,
        max_index_bytes=max_index_bytes,
    )
    bootstrap_paths = {
        ".archbirdignore",
        ".gitignore",
        ".ignore",
        "Makefile",
        "DESCRIPTION",
        "NAMESPACE",
        "configure.ac",
        "package.json",
        "pyproject.toml",
        "setup.cfg",
        "CMakeLists.txt",
    }
    bootstrap_overlay = {
        path: data for path, data in overlay.items() if path in bootstrap_paths
    }
    bootstrap_rows = _overlay_rows(
        _root_rows(repository, normalized_transient_exclude),
        bootstrap_overlay,
    )
    bootstrap_inventory = _repository_inventory(
        repository,
        bootstrap_rows,
        include_standard_ignores=ignore,
        ignore_files=normalized_ignore_files,
        overlay=overlay,
    )
    bootstrap = json.loads(
        _native.discovery_resolve(config_json, request, bootstrap_inventory)
    )
    effective = _canonical(bootstrap["effective_config"])
    rows, pruned_directories = _inventory_rows(
        effective,
        repository,
        # Disk ignore rules cannot safely prune the virtual after-state when
        # the overlay changes one. Enumerate exhaustively and let the native
        # resolver apply the overlaid ignore documents below.
        include_standard_ignores=ignore and not ignore_inputs_changed,
        ignore_files=(
            () if ignore_inputs_changed else normalized_ignore_files
        ),
        transient_exclude=normalized_transient_exclude,
    )
    final_overlay = {
        path: data
        for path, data in overlay.items()
        if path not in normalized_transient_exclude
    }
    rows = _overlay_rows(rows, final_overlay)
    candidate_inventory = _repository_inventory(
        repository,
        rows,
        include_standard_ignores=ignore,
        ignore_files=normalized_ignore_files,
        pruned_directories=pruned_directories,
        overlay=overlay,
    )
    candidate_resolution = json.loads(
        _native.discovery_resolve(
            config_json, request, candidate_inventory
        )
    )
    manifest_requests = candidate_resolution.get("manifest_requests")
    if not isinstance(manifest_requests, list):
        raise ConfigError("native resolution has no manifest request ledger")
    inventory = _repository_inventory(
        repository,
        rows,
        include_standard_ignores=ignore,
        ignore_files=normalized_ignore_files,
        pruned_directories=pruned_directories,
        overlay=overlay,
        manifest_requests=manifest_requests,
    )
    return _native.discovery_resolve(
        config_json, request, inventory, pretty=pretty
    )


def _read_sources(
    root: Path,
    plan: Mapping[str, object],
    *,
    overlay: Optional[Mapping[str, Optional[bytes]]] = None,
) -> Tuple[Source, ...]:
    max_file_bytes = int(plan["max_file_bytes"])
    max_index_bytes = int(plan["max_index_bytes"])
    sources = []
    for row in plan["files"]:
        path = str(row["path"])
        roles = tuple(str(role) for role in row["roles"])
        is_index = "index" in roles
        byte_limit = max_index_bytes if is_index else max_file_bytes
        limit_name = "max_index_bytes" if is_index else "max_file_bytes"
        if overlay is not None and path in overlay:
            data = overlay[path]
            if data is None:
                raise ConfigError(
                    f"selected source was removed by source overlay: {path}"
                )
        else:
            candidate = root / path
            try:
                metadata = candidate.lstat()
            except OSError as error:
                raise ConfigError(
                    f"cannot stat selected source: {path}: {error}"
                ) from error
            if not stat.S_ISREG(metadata.st_mode):
                raise ConfigError(
                    f"selected source is no longer a regular file: {path}"
                )
            if metadata.st_size > byte_limit:
                raise ConfigError(
                    f"selected {'index' if is_index else 'source'} exceeds "
                    f"limits.{limit_name}: {path}: {metadata.st_size} > "
                    f"{byte_limit}"
                )
            try:
                data = candidate.read_bytes()
            except OSError as error:
                raise ConfigError(
                    f"cannot read selected source: {path}: {error}"
                ) from error
        assert data is not None
        if len(data) > byte_limit:
            raise ConfigError(
                f"selected {'index' if is_index else 'source'} exceeds "
                f"limits.{limit_name} while reading: {path}"
            )
        sources.append(
            Source(
                path,
                data,
                language=str(row["language"]),
                layer=str(row["layer"]),
                roles=roles,
            )
        )
    return tuple(sources)


__all__ = [
    "Project",
    "PATTERN_CONTRACT",
    "PATTERN_CONTRACT_VERSION",
    "PATTERN_ENGINE",
    "PATTERN_OPTIONS",
    "PATTERN_UNICODE",
    "Source",
    "Workspace",
    "analyze_okf_source",
    "analyze_workspace_json",
    "accept_act_json",
    "audit_map_freshness",
    "compile_plan_json",
    "compile_project_configuration",
    "compile_query_plan_json",
    "compile_test_observations",
    "diff_maps_json",
    "evaluate_constraints_json",
    "evaluate_projection_json",
    "export_graph",
    "export_okf_bundle",
    "freeze_constraints_json",
    "materialize_act_json",
    "act_source_requirements",
    "plan_source_requirements",
    "preflight_act_apply",
    "publish_okf_bundle",
    "query_map_markdown",
    "query_map_json",
    "path_map_json",
    "path_map_markdown",
    "render_path_markdown",
    "render_map_markdown",
    "render_plan_markdown",
    "render_source_markdown",
    "resolve_discovery",
    "validate_act",
    "validate_plan",
    "validate_test_symbol_observations",
    "write_okf_bundle",
]
