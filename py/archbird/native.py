"""Python host for the language-neutral native Archbird core.

The host owns source discovery/classification and CPython AST execution.  Exact
source bytes and normalized provider facts cross into the I/O-free C kernel.
"""

from __future__ import annotations

from collections import deque
from concurrent.futures import Future, ProcessPoolExecutor
from dataclasses import dataclass
import html
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tempfile
import unicodedata
from typing import Callable, Iterable, Mapping, Optional, Sequence, Tuple, Union

from . import _native
from .errors import ConfigError
from .provider_cache import ProviderCache
from .providers import (
    python_ast_implementation_sha256,
    python_ast_provider_facts,
    python_verification_fact,
)


PATTERN_CONTRACT_VERSION = _native.PATTERN_CONTRACT_VERSION
CORE_IMPLEMENTATION_SHA256 = _native.IMPLEMENTATION_SHA256
PATTERN_CONTRACT = _native.PATTERN_CONTRACT
PATTERN_ENGINE = _native.PATTERN_ENGINE
PATTERN_UNICODE = _native.PATTERN_UNICODE
PATTERN_OPTIONS = _native.PATTERN_OPTIONS

_NATIVE_CACHE_PROVIDERS = (
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
        text=data.decode("utf-8"),
    )


def _python_provider_chunk(
    tasks: Sequence[Tuple[str, str, bytes]],
) -> Tuple[bytes, ...]:
    return tuple(_python_provider_task(task) for task in tasks)


def _parallel_python_providers(
    tasks: Sequence[Tuple[str, str, bytes]], workers: int
) -> Iterable[bytes]:
    """Yield provider bundles in input order with bounded process fan-out."""

    chunk_size = max(1, len(tasks) // (workers * 4))
    chunks = (
        tasks[start : start + chunk_size]
        for start in range(0, len(tasks), chunk_size)
    )
    with ProcessPoolExecutor(max_workers=workers) as executor:
        pending: deque[Future[Tuple[bytes, ...]]] = deque()
        for _ in range(workers * 2):
            try:
                pending.append(executor.submit(_python_provider_chunk, next(chunks)))
            except StopIteration:
                break
        while pending:
            yield from pending.popleft().result()
            try:
                pending.append(executor.submit(_python_provider_chunk, next(chunks)))
            except StopIteration:
                pass


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


def _native_cache_namespace() -> str:
    return hashlib.sha256(
        b"archbird-native-provider-cache-v1\0"
        + CORE_IMPLEMENTATION_SHA256.encode("ascii")
    ).hexdigest()


def _python_ast_cache_namespace() -> str:
    identity = (
        f"archbird-python-ast-cache-v1\0"
        f"{python_ast_implementation_sha256()}\0"
        f"cpython-{sys.version_info.major}.{sys.version_info.minor}."
        f"{sys.version_info.micro}"
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
        self.cache_stats: Mapping[str, int] = {
            "errors": 0,
            "hits": 0,
            "invalid": 0,
            "misses": 0,
            "writes": 0,
        }
        classifications = [
            {
                "language": source.language,
                "layer": source.layer,
                "path": source.path,
                "roles": sorted(set(source.roles)),
            }
            for source in ordered
        ]
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
        cache_dir: Optional[Union[str, Path]] = None,
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
        inventory = _inventory(config_json, repository)
        plan = json.loads(_native.discovery_plan(config_json, inventory))
        sources = _read_sources(repository, plan)
        project = cls(
            plan["project"],
            sources,
            configuration_sha256=plan["configuration_sha256"],
        )
        project.root = repository
        project.set_config(config_json)
        if scan:
            project.scan(jobs=jobs, cache_dir=cache_dir)
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
        cache_dir: Optional[Union[str, Path]] = None,
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
        current.set_config(effective_config)
        if scan:
            current.scan(jobs=jobs, cache_dir=cache_dir)
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
        return _native.project_counts(self._capsule)

    @property
    def config_sha256(self) -> str:
        return _native.project_config_sha256(self._capsule)

    def set_config(self, config_json: bytes) -> None:
        _native.project_set_config(self._capsule, config_json)

    def add_provider(self, provider_json: bytes, mode: str = "primary") -> None:
        if self._providers_finalized:
            raise RuntimeError("providers are already finalized")
        _native.project_add_provider(self._capsule, mode, provider_json)

    def add_test_symbol_observations(self, observations_json: bytes) -> None:
        """Attach strict project-runner evidence without changing static facts."""

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

    def scan(
        self,
        mode: str = "primary",
        *,
        jobs: int = 0,
        cache_dir: Optional[Union[str, Path]] = None,
        progress: Optional[Callable[[Mapping[str, object]], None]] = None,
    ) -> None:
        """Compose lexical, portable syntax, and CPython AST evidence."""

        if self._providers_finalized:
            raise RuntimeError("providers are already finalized")
        if jobs < 0:
            raise ValueError("jobs must be zero or positive")
        support_mode = "augment" if mode == "primary" else mode
        cache = ProviderCache(cache_dir) if cache_dir is not None else None

        def report(**event: object) -> None:
            if progress is not None:
                progress(event)

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
            # C lexical facts still depend on configured public headers and
            # remain deliberately manifest-bound until that closure is explicit.
            report(phase="providers", provider="lexical:c", state="start")
            _native.project_scan_builtin_provider(
                self._capsule, "lexical:c", support_mode
            )
            report(phase="providers", provider="lexical:c", state="complete")
            namespace = _native_cache_namespace()
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
        if workers == 1 or len(python_sources) < 2:
            bundles = map(_python_provider_task, tasks)
        else:
            bundles = _parallel_python_providers(tasks, workers)
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
        self.cache_stats = cache.stats.as_dict() if cache is not None else {
            "errors": 0,
            "hits": 0,
            "invalid": 0,
            "misses": 0,
            "writes": 0,
        }

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
        return _native.project_file_facts(self._capsule, pretty=pretty)

    def file_facts(self) -> Mapping[str, object]:
        return json.loads(self.file_facts_json())

    def merge_ledger_json(self, *, pretty: bool = False) -> bytes:
        return _native.project_merge_ledger(self._capsule, pretty=pretty)

    def merge_conflicts_json(self, *, pretty: bool = False) -> bytes:
        """Render the bounded conflict-only provider ledger."""

        return _native.project_merge_conflicts(self._capsule, pretty=pretty)

    def merge_summary(self) -> Mapping[str, int]:
        return _native.project_merge_summary(self._capsule)

    def provider_facts_json(self, index: int, *, pretty: bool = False) -> bytes:
        """Render one normalized provider bundle by deterministic index."""

        return _native.project_provider_facts(
            self._capsule, index, pretty=pretty
        )

    def provider_facts(self, index: int) -> Mapping[str, object]:
        return json.loads(self.provider_facts_json(index))

    def map_json(self, *, pretty: bool = False) -> bytes:
        return _native.project_map(self._capsule, pretty=pretty)

    def map(self) -> Mapping[str, object]:
        return json.loads(self.map_json())

    def map_markdown(
        self,
        *,
        view: str = "overview",
        detail: str = "standard",
        full: bool = False,
        max_chars: int = 0,
    ) -> bytes:
        """Render a human view of the current complete canonical Map."""

        return render_map_markdown(
            self.map_json(),
            view=view,
            detail=detail,
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
            context=context,
            direction=direction,
            depth=depth,
            test_depth=test_depth,
            pretty=pretty,
        )

    def query(self, **kwargs: object) -> Mapping[str, object]:
        return json.loads(self.query_json(**kwargs))

    def query_markdown(
        self,
        *,
        focus: Sequence[str] = (),
        paths: Sequence[str] = (),
        symbols: Sequence[str] = (),
        components: Sequence[str] = (),
        packages: Sequence[str] = (),
        artifacts: Sequence[str] = (),
        context: Optional[Mapping[str, object]] = None,
        direction: str = "both",
        depth: int = 1,
        test_depth: int = 8,
        max_chars: int = 0,
    ) -> bytes:
        """Render a ranked, whole-file query neighborhood as Markdown."""

        return query_map_markdown(
            self.map_json(),
            focus=focus,
            paths=paths,
            symbols=symbols,
            components=components,
            packages=packages,
            artifacts=artifacts,
            context=context,
            direction=direction,
            depth=depth,
            test_depth=test_depth,
            max_chars=max_chars,
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
        cache_dir: Optional[Union[str, Path]] = None,
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
                    cache_dir=cache_dir,
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


class Verification:
    """Host-loaded evidence evaluated by the I/O-free native Verify core."""

    def __init__(self, suite_json: bytes, input_json: bytes, *, path: Path) -> None:
        self.suite_json = bytes(suite_json)
        self.input_json = bytes(input_json)
        self.path = path

    @classmethod
    def from_config(
        cls,
        suite_path: Union[str, Path],
        *,
        project_roots: Optional[Mapping[str, Union[str, Path]]] = None,
        baseline: Optional[Union[str, Path]] = None,
        jobs: int = 0,
        cache_dir: Optional[Union[str, Path]] = None,
    ) -> "Verification":
        """Load only the paths declared by a validated verification plan."""

        path = Path(suite_path).resolve()
        try:
            suite_json = path.read_bytes()
        except OSError as error:
            raise ConfigError(f"cannot read verification suite: {path}: {error}") from error
        plan = json.loads(verification_plan_json(suite_json))
        suite = _strict_document(suite_json, "verification suite")
        base = path.parent
        overrides = {
            str(name): Path(value).resolve()
            for name, value in (project_roots or {}).items()
        }
        expected_names = {str(row["name"]) for row in plan["projects"]}
        unknown_overrides = sorted(set(overrides) - expected_names)
        if unknown_overrides:
            raise ConfigError(
                "unknown verification project root overrides: "
                + ", ".join(unknown_overrides)
            )
        source_plan: dict[str, list[Mapping[str, object]]] = {}
        for row in plan["sources"]:
            source_plan.setdefault(str(row["project"]), []).append(row)

        projects = []
        project_inputs: dict[str, dict[str, object]] = {}
        repository_roots: dict[str, Optional[Path]] = {}
        source_text: dict[tuple[str, str], str] = {}
        for row in plan["projects"]:
            name = str(row["name"])
            asserted_root = (
                (base / str(row["root"])).resolve()
                if row["root"] is not None
                else None
            )
            root = overrides.get(name, asserted_root)
            project: Optional[Project] = None
            if row["config"] is not None:
                config_path = (base / str(row["config"])).resolve()
                project = Project.from_config(
                    config_path, root=root, jobs=jobs, cache_dir=cache_dir
                )
                map_document = _strict_document(
                    project.map_json(), f"project {name} map"
                )
                repository_root = project.root
                available = {source.path: source.data for source in project.sources}
            else:
                map_path = (base / str(row["map"])).resolve()
                try:
                    map_bytes = map_path.read_bytes()
                except OSError as error:
                    raise ConfigError(
                        f"cannot read project {name} map: {map_path}: {error}"
                    ) from error
                map_document = _strict_document(map_bytes, f"project {name} map")
                repository_root = root
                available = {}

            sources = []
            loaded_source_paths: set[str] = set()
            for source_row in source_plan.get(name, []):
                source_path = str(source_row["path"])
                if source_path in loaded_source_paths:
                    continue
                data = available.get(source_path)
                if data is None:
                    if repository_root is None:
                        raise ConfigError(
                            f"project {name}: verification source {source_path!r} "
                            "requires a suite root or --project-root override"
                        )
                    candidate = (repository_root / source_path).resolve()
                    try:
                        candidate.relative_to(repository_root.resolve())
                    except ValueError as error:
                        raise ConfigError(
                            f"project {name}: source path escapes root: {source_path}"
                        ) from error
                    try:
                        data = candidate.read_bytes()
                    except OSError as error:
                        raise ConfigError(
                            f"project {name}: cannot read {source_path}: {error}"
                        ) from error
                try:
                    text = data.decode("utf-8")
                except UnicodeDecodeError as error:
                    raise ConfigError(
                        f"project {name}: invalid UTF-8 in {source_path}: {error}"
                    ) from error
                source_text[(name, source_path)] = text
                sources.append({"path": source_path, "text": text})
                loaded_source_paths.add(source_path)
            # Map-backed extractors can cite configuration/build/provider paths
            # that are intentionally absent from the public file inventory.
            # Preserve current-byte evidence for every configured manifest
            # input without decoding or duplicating its text in the envelope.
            for source_path, data in sorted(available.items()):
                if source_path in loaded_source_paths:
                    continue
                sources.append(
                    {
                        "path": source_path,
                        "sha256": hashlib.sha256(data).hexdigest(),
                    }
                )
                loaded_source_paths.add(source_path)
            sources.sort(key=lambda source: str(source["path"]))
            project_input = {
                "name": name,
                "map": map_document,
                "sources": sources,
            }
            projects.append(project_input)
            project_inputs[name] = project_input
            repository_roots[name] = repository_root

        extractors = suite.get("extractors", {})
        provided_facts = []
        for row in plan["sources"]:
            if row["provider"] != "python-ast":
                continue
            name = str(row["extractor"])
            project_name = str(row["project"])
            source_path = str(row["path"])
            provided_facts.append(
                python_verification_fact(
                    name=name,
                    spec=extractors[name],
                    project=project_name,
                    path=source_path,
                    text=source_text[(project_name, source_path)],
                )
            )

        attestations = []
        for row in plan["attestations"]:
            attestation_path = (base / str(row["path"])).resolve()
            try:
                raw = attestation_path.read_bytes()
            except OSError as error:
                message = f"cannot read attestation {attestation_path}: {error}"
                attestations.append(
                    {
                        "name": str(row["name"]),
                        "path": str(row["path"]),
                        "error": message.replace(
                            str(attestation_path), str(row["path"])
                        ),
                    }
                )
                continue
            try:
                document = _strict_document(
                    raw, f"verification attestation {row['name']}"
                )
            except ConfigError as error:
                attestations.append(
                    {
                        "name": str(row["name"]),
                        "path": str(row["path"]),
                        "error": str(error).replace(
                            str(attestation_path), str(row["path"])
                        ),
                    }
                )
                continue
            attestations.append(
                {
                    "name": str(row["name"]),
                    "path": str(row["path"]),
                    "document": document,
                }
            )
            producer = document.get("producer")
            evidence_rows = (
                producer.get("evidence", [])
                if isinstance(producer, Mapping)
                else []
            )
            project_name = str(row["project"])
            repository_root = repository_roots[project_name]
            if repository_root is None or not isinstance(evidence_rows, list):
                continue
            project_sources = project_inputs[project_name]["sources"]
            assert isinstance(project_sources, list)
            known_paths = {
                str(source["path"])
                for source in project_sources
                if isinstance(source, Mapping) and "path" in source
            }
            for evidence in evidence_rows:
                if not isinstance(evidence, Mapping) or not isinstance(
                    evidence.get("path"), str
                ):
                    continue
                evidence_path = str(evidence["path"])
                if evidence_path in known_paths:
                    continue
                candidate = (repository_root / evidence_path).resolve()
                try:
                    candidate.relative_to(repository_root.resolve())
                except ValueError:
                    project_sources.append(
                        {"path": evidence_path, "error": "path escapes project root"}
                    )
                    known_paths.add(evidence_path)
                    continue
                try:
                    evidence_bytes = candidate.read_bytes()
                except OSError as error:
                    portable_error = str(error).replace(str(candidate), evidence_path)
                    project_sources.append(
                        {"path": evidence_path, "error": portable_error}
                    )
                else:
                    project_sources.append(
                        {
                            "path": evidence_path,
                            "sha256": hashlib.sha256(evidence_bytes).hexdigest(),
                        }
                    )
                known_paths.add(evidence_path)
            project_sources.sort(key=lambda source: str(source["path"]))

        baseline_document = None
        if baseline is not None:
            baseline_path = Path(baseline).resolve()
            try:
                raw_baseline = baseline_path.read_bytes()
            except OSError as error:
                raise ConfigError(
                    f"cannot read verification baseline: {baseline_path}: {error}"
                ) from error
            baseline_document = _strict_document(raw_baseline, "verification baseline")

        input_document = {
            "schema_version": 1,
            "artifact": "verification-input",
            "suite_path": path.name,
            "projects": projects,
            "provided_facts": provided_facts,
            "attestations": attestations,
            "baseline": baseline_document,
        }
        return cls(suite_json, _canonical(input_document), path=path)

    def result_json(self, *, pretty: bool = False) -> bytes:
        return verification_analyze_json(
            self.suite_json, self.input_json, pretty=pretty
        )

    def result(self) -> Mapping[str, object]:
        return json.loads(self.result_json())

    def report(
        self,
        format: str,
        *,
        full: bool = False,
        max_findings: int = 200,
        pretty: bool = True,
    ) -> bytes:
        """Render a complete machine report or bounded Markdown projection."""

        if format == "json":
            return self.result_json(pretty=pretty)
        if max_findings < 0:
            raise ValueError("max_findings must be nonnegative")
        return verification_report(
            self.suite_json,
            self.input_json,
            format=format,
            max_findings=-1 if full else max_findings,
            pretty=pretty,
        )

    def has_errors(self) -> bool:
        return bool(self.result()["summary"]["blocking"])


def _query_request(
    *,
    focus: Sequence[str] = (),
    paths: Sequence[str] = (),
    symbols: Sequence[str] = (),
    components: Sequence[str] = (),
    packages: Sequence[str] = (),
    artifacts: Sequence[str] = (),
    context: Optional[Mapping[str, object]] = None,
    direction: str = "both",
    depth: int = 1,
    test_depth: int = 8,
) -> bytes:
    request = {
        "artifacts": list(artifacts),
        "components": list(components),
        "depth": depth,
        "direction": direction,
        "focus": list(focus),
        "packages": list(packages),
        "paths": list(paths),
        "symbols": list(symbols),
        "test_depth": test_depth,
    }
    if context is not None:
        request["context"] = dict(context)
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
    context: Optional[Mapping[str, object]] = None,
    direction: str = "both",
    depth: int = 1,
    test_depth: int = 8,
    pretty: bool = False,
) -> bytes:
    request = _query_request(
        focus=focus,
        paths=paths,
        symbols=symbols,
        components=components,
        context=context,
        packages=packages,
        artifacts=artifacts,
        direction=direction,
        depth=depth,
        test_depth=test_depth,
    )
    return _native.map_query(map_json, request, pretty=pretty)


def render_map_markdown(
    map_json: bytes,
    *,
    view: str = "overview",
    detail: str = "standard",
    full: bool = False,
    max_chars: int = 0,
) -> bytes:
    """Project a canonical saved Map without changing its complete IR."""

    if max_chars < 0:
        raise ValueError("max_chars must be nonnegative")
    views = {"overview": 0, "architecture": 1, "audit": 2}
    details = {"compact": 0, "standard": 1, "full": 2}
    if view not in views:
        raise ValueError("view must be overview, architecture, or audit")
    if detail not in details:
        raise ValueError("detail must be compact, standard, or full")
    if full:
        if detail != "standard":
            raise ValueError("full and an explicit non-standard detail conflict")
        detail = "full"
    return _native.map_markdown_view(
        map_json, views[view], details[detail], max_chars=max_chars
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
    context: Optional[Mapping[str, object]] = None,
    direction: str = "both",
    depth: int = 1,
    test_depth: int = 8,
    max_chars: int = 0,
) -> bytes:
    """Query a canonical saved Map and render bounded Markdown context."""

    if max_chars < 0:
        raise ValueError("max_chars must be nonnegative")
    request = _query_request(
        focus=focus,
        paths=paths,
        symbols=symbols,
        components=components,
        context=context,
        packages=packages,
        artifacts=artifacts,
        direction=direction,
        depth=depth,
        test_depth=test_depth,
    )
    return _native.map_query_markdown(
        map_json, request, max_chars=max_chars
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
    proposal_json: bytes = b"",
    contract_json: bytes = b"",
    result_json: bytes = b"",
    normalization_json: Optional[bytes] = None,
    pretty: bool = False,
) -> bytes:
    """Project canonical artifacts into a native content-addressed OKF bundle."""

    artifacts = tuple(
        bytes(value)
        for value in (
            map_json,
            verification_json,
            proposal_json,
            contract_json,
            result_json,
        )
    )
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
    proposal_path: Optional[Union[str, Path]] = None,
    contract_path: Optional[Union[str, Path]] = None,
    result_path: Optional[Union[str, Path]] = None,
    replace: bool = False,
) -> bytes:
    """Read stable canonical inputs, publish, and atomically install OKF."""

    map_source = Path(map_path).resolve()
    optional_sources = tuple(
        Path(value).resolve() if value is not None else None
        for value in (verification_path, proposal_path, contract_path, result_path)
    )
    paths = [map_source, *(path for path in optional_sources if path is not None)]
    before_by_path = {path: path.read_bytes() for path in paths}
    bundle = publish_okf_bundle(
        before_by_path[map_source],
        verification_json=(
            before_by_path[optional_sources[0]] if optional_sources[0] else b""
        ),
        proposal_json=(
            before_by_path[optional_sources[1]] if optional_sources[1] else b""
        ),
        contract_json=(
            before_by_path[optional_sources[2]] if optional_sources[2] else b""
        ),
        result_json=(
            before_by_path[optional_sources[3]] if optional_sources[3] else b""
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


def verification_plan_json(
    suite_json: bytes, *, pretty: bool = False
) -> bytes:
    """Validate a suite and return the native host-loading plan."""

    return _native.verification_plan(suite_json, pretty=pretty)


def verification_analyze_json(
    suite_json: bytes, input_json: bytes, *, pretty: bool = False
) -> bytes:
    """Evaluate one reviewed suite over a host-built verification input."""

    return _native.verification_analyze(
        suite_json, input_json, pretty=pretty
    )


def verification_report(
    suite_json: bytes,
    input_json: bytes,
    *,
    format: str,
    max_findings: int = 200,
    pretty: bool = True,
) -> bytes:
    """Render Markdown, SARIF, or JUnit through the shared native core."""

    return _native.verification_report(
        suite_json,
        input_json,
        format,
        max_findings=max_findings,
        pretty=pretty,
    )


def change_proposal(
    verification_json: bytes,
    fingerprint: str,
    *,
    format: str = "json",
    full: bool = False,
    max_candidates: int = 100,
    pretty: bool = True,
) -> bytes:
    """Derive one sealed proposal from one exact verification finding."""

    if max_candidates < 0:
        raise ValueError("max_candidates must be nonnegative")
    return _native.change_proposal(
        verification_json,
        fingerprint,
        format=format,
        full=full,
        max_candidates=max_candidates,
        pretty=pretty,
    )


def change_contract(
    proposal_json: bytes,
    *,
    objective: str,
    owner: str,
    rationale: str,
    preserve_checks: Sequence[str] = (),
    selected_candidates: Sequence[str] = (),
    format: str = "json",
    pretty: bool = True,
) -> bytes:
    """Seal explicit review metadata as an asserted change contract."""

    review = {
        "objective": objective,
        "owner": owner,
        "rationale": rationale,
        "preserve_checks": list(preserve_checks),
        "selected_candidates": list(selected_candidates),
    }
    return _native.change_contract(
        proposal_json, _canonical(review), format=format, pretty=pretty
    )


def change_verify(
    proposal_json: bytes,
    contract_json: bytes,
    before_verification_json: bytes,
    after_verification_json: bytes,
    *,
    format: str = "json",
    pretty: bool = True,
) -> bytes:
    """Judge an asserted fact transition without executing or editing a project."""

    return _native.change_verify(
        proposal_json,
        contract_json,
        before_verification_json,
        after_verification_json,
        format=format,
        pretty=pretty,
    )


@dataclass(frozen=True)
class ChangeProposal:
    """Immutable derived proposal bytes with review helpers."""

    json_bytes: bytes

    @classmethod
    def compile(cls, verification_json: bytes, fingerprint: str) -> "ChangeProposal":
        return cls(
            change_proposal(
                verification_json, fingerprint, format="json", pretty=False
            )
        )

    def data(self) -> Mapping[str, object]:
        return json.loads(self.json_bytes)

    def report(
        self,
        verification_json: bytes,
        *,
        full: bool = False,
        max_candidates: int = 100,
    ) -> bytes:
        fingerprint = str(self.data()["origin"]["finding"]["fingerprint"])
        regenerated = change_proposal(
            verification_json,
            fingerprint,
            format="json",
            pretty=False,
        )
        if json.loads(regenerated)["sha256"] != self.data()["sha256"]:
            raise ConfigError("change proposal no longer matches verification evidence")
        return change_proposal(
            verification_json,
            fingerprint,
            format="markdown",
            full=full,
            max_candidates=max_candidates,
        )

    def review(
        self,
        *,
        objective: str,
        owner: str,
        rationale: str,
        preserve_checks: Sequence[str] = (),
        selected_candidates: Sequence[str] = (),
    ) -> "ChangeContract":
        return ChangeContract(
            proposal_json=self.json_bytes,
            json_bytes=change_contract(
                self.json_bytes,
                objective=objective,
                owner=owner,
                rationale=rationale,
                preserve_checks=preserve_checks,
                selected_candidates=selected_candidates,
                format="json",
                pretty=False,
            ),
        )


@dataclass(frozen=True)
class ChangeContract:
    """Immutable asserted contract bound to its exact proposal."""

    proposal_json: bytes
    json_bytes: bytes

    def data(self) -> Mapping[str, object]:
        return json.loads(self.json_bytes)

    def verify(
        self,
        before_verification_json: bytes,
        after_verification_json: bytes,
        *,
        format: str = "json",
        pretty: bool = True,
    ) -> bytes:
        return change_verify(
            self.proposal_json,
            self.json_bytes,
            before_verification_json,
            after_verification_json,
            format=format,
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
) -> tuple[Tuple[str, ...], Tuple[str, ...], Tuple[Tuple[str, int], ...]]:
    files: list[str] = []
    file_sizes: list[tuple[str, int]] = []
    pruned: list[str] = []
    pending: list[tuple[str, Path]] = [("", root)]
    standard_ignores: list[tuple[str, bytes]] = []
    custom_paths = tuple(ignore_files)
    custom_set = set(custom_paths)
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


def _inventory(config_json: bytes, root: Path) -> Tuple[str, ...]:
    return _inventory_state(config_json, root)[0]


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
        metadata = candidate.stat(follow_symlinks=False)
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
) -> tuple[list[dict[str, object]], Tuple[str, ...]]:
    _paths, pruned, file_sizes = _inventory_state(
        config_json,
        root,
        include_standard_ignores=include_standard_ignores,
        ignore_files=ignore_files,
        collect_sizes=True,
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
        raise ConfigError(f"ignore file is outside repository root: {value}") from error
    if not relative or relative == ".":
        raise ConfigError(f"ignore file is not a repository file: {value}")
    return relative


def _ignore_sort_key(path: str) -> tuple[int, tuple[str, ...], int, str]:
    parts = PurePosixPath(path).parts
    priority = {".gitignore": 0, ".ignore": 1, ".archbirdignore": 2}.get(
        parts[-1], 3
    )
    return (len(parts) - 1, parts[:-1], priority, path)


def _encoded_input(root: Path, path: str) -> dict[str, str]:
    candidate = root.joinpath(*PurePosixPath(path).parts)
    try:
        data = candidate.read_bytes()
    except OSError as error:
        raise ConfigError(f"cannot read discovery input: {path}: {error}") from error
    return {"content_hex": data.hex(), "path": path}


def _repository_inventory(
    root: Path,
    rows: Sequence[Mapping[str, object]],
    *,
    include_standard_ignores: bool,
    ignore_files: Sequence[Union[str, Path]],
    pruned_directories: Sequence[str] = (),
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
        documents.append(_encoded_input(root, "package.json"))
    if "pyproject.toml" in paths:
        documents.append(_encoded_input(root, "pyproject.toml"))
    return _canonical(
        {
            "artifact": "archbird-repository-inventory",
            "documents": documents,
            "files": list(rows),
            "ignore_files": [_encoded_input(root, path) for path in selected],
            "pruned_directories": list(pruned_directories),
            "schema_version": 1,
        }
    )


def _root_rows(root: Path) -> list[dict[str, object]]:
    paths = (
        ".archbirdignore",
        ".gitignore",
        ".ignore",
        "Makefile",
        "package.json",
        "pyproject.toml",
    )
    return [row for path in paths if (row := _file_row(root, path)) is not None]


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
) -> bytes:
    """Return the canonical C-owned config-resolution artifact for a repository."""

    repository = Path(root).resolve()
    if not repository.is_dir():
        raise ConfigError(f"root is not a directory: {repository}")
    config_json = _config_bytes(config)
    normalized_ignore_files = tuple(
        dict.fromkeys(_safe_relative(repository, value) for value in ignore_files)
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
    bootstrap_rows = _root_rows(repository)
    bootstrap_inventory = _repository_inventory(
        repository,
        bootstrap_rows,
        include_standard_ignores=ignore,
        ignore_files=normalized_ignore_files,
    )
    bootstrap = json.loads(
        _native.discovery_resolve(config_json, request, bootstrap_inventory)
    )
    effective = _canonical(bootstrap["effective_config"])
    rows, pruned_directories = _inventory_rows(
        effective,
        repository,
        include_standard_ignores=ignore,
        ignore_files=normalized_ignore_files,
    )
    inventory = _repository_inventory(
        repository,
        rows,
        include_standard_ignores=ignore,
        ignore_files=normalized_ignore_files,
        pruned_directories=pruned_directories,
    )
    return _native.discovery_resolve(
        config_json, request, inventory, pretty=pretty
    )


def _read_sources(root: Path, plan: Mapping[str, object]) -> Tuple[Source, ...]:
    max_file_bytes = int(plan["max_file_bytes"])
    max_index_bytes = int(plan["max_index_bytes"])
    sources = []
    for row in plan["files"]:
        path = str(row["path"])
        roles = tuple(str(role) for role in row["roles"])
        is_index = "index" in roles
        byte_limit = max_index_bytes if is_index else max_file_bytes
        limit_name = "max_index_bytes" if is_index else "max_file_bytes"
        candidate = root / path
        try:
            metadata = candidate.stat(follow_symlinks=False)
        except OSError as error:
            raise ConfigError(f"cannot stat selected source: {path}: {error}") from error
        if not stat.S_ISREG(metadata.st_mode):
            raise ConfigError(f"selected source is no longer a regular file: {path}")
        if metadata.st_size > byte_limit:
            raise ConfigError(
                f"selected {'index' if is_index else 'source'} exceeds "
                f"limits.{limit_name}: {path}: {metadata.st_size} > {byte_limit}"
            )
        try:
            data = candidate.read_bytes()
        except OSError as error:
            raise ConfigError(f"cannot read selected source: {path}: {error}") from error
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
    "ChangeContract",
    "ChangeProposal",
    "Project",
    "PATTERN_CONTRACT",
    "PATTERN_CONTRACT_VERSION",
    "PATTERN_ENGINE",
    "PATTERN_OPTIONS",
    "PATTERN_UNICODE",
    "Source",
    "Verification",
    "Workspace",
    "analyze_workspace_json",
    "change_contract",
    "change_proposal",
    "change_verify",
    "diff_maps_json",
    "export_graph",
    "query_map_json",
    "resolve_discovery",
    "verification_analyze_json",
    "verification_plan_json",
    "verification_report",
]
