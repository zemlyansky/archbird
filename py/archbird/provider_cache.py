"""Persistent content-addressed cache for normalized provider-facts bundles."""

from __future__ import annotations

from dataclasses import dataclass
import errno
import hashlib
import os
from pathlib import Path
import re
import socket
import sys
import tempfile
from typing import BinaryIO, Mapping


_CACHE_CONTRACT = b"archbird-provider-cache-v1"
_MAP_CACHE_CONTRACT = b"archbird-map-result-cache-v1"
_DEFAULT_MAX_BYTES = 1024 * 1024 * 1024
_MAX_SAFE_INTEGER = (1 << 53) - 1


def _temporary_domain() -> str:
    identity = [socket.gethostname(), sys.platform]
    for candidate, link in (
        (Path("/proc/sys/kernel/random/boot_id"), False),
        (Path("/proc/self/ns/pid"), True),
    ):
        try:
            value = (
                os.readlink(candidate)
                if link
                else candidate.read_text(encoding="ascii").strip()
            )
        except OSError:
            value = ""
        identity.append(value)
    return hashlib.sha256("\0".join(identity).encode("utf-8")).hexdigest()[:16]


_TEMPORARY_DOMAIN = _temporary_domain()
_TEMPORARY_NAME = re.compile(
    r"^\..+\.([0-9a-f]{16})\.([1-9][0-9]*)\.([A-Za-z0-9_-]+)\.tmp$"
)


def default_provider_cache_dir() -> Path:
    """Return the host cache root without binding artifacts to absolute paths."""

    configured = os.environ.get("ARCHBIRD_CACHE_DIR")
    if configured:
        return Path(configured).expanduser()
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return Path(xdg).expanduser() / "archbird"
    return Path.home() / ".cache" / "archbird"


def default_provider_cache_max_bytes() -> int:
    """Return the bounded host-cache budget."""

    configured = os.environ.get("ARCHBIRD_CACHE_MAX_BYTES")
    if configured is None:
        return _DEFAULT_MAX_BYTES
    if not configured or any(
        character not in "0123456789" for character in configured
    ):
        raise ValueError(
            "ARCHBIRD_CACHE_MAX_BYTES must be a positive safe integer"
        )
    value = int(configured, 10)
    if value <= 0 or value > _MAX_SAFE_INTEGER:
        raise ValueError(
            "ARCHBIRD_CACHE_MAX_BYTES must be a positive safe integer"
        )
    return value


def _framed(digest: "hashlib._Hash", value: bytes) -> None:
    digest.update(len(value).to_bytes(8, "big"))
    digest.update(value)


def _cache_key(
    *,
    namespace: str,
    project: str,
    provider_id: str,
    path: str,
    source_sha256: str,
) -> str:
    digest = hashlib.sha256()
    for value in (
        _CACHE_CONTRACT,
        namespace.encode("ascii"),
        project.encode("utf-8"),
        provider_id.encode("utf-8"),
        path.encode("utf-8"),
        source_sha256.encode("ascii"),
    ):
        _framed(digest, value)
    return digest.hexdigest()


def _map_cache_key(
    *, namespace: str, project: str, manifest_sha256: str, config_sha256: str
) -> str:
    digest = hashlib.sha256()
    for value in (
        _MAP_CACHE_CONTRACT,
        namespace.encode("ascii"),
        project.encode("utf-8"),
        manifest_sha256.encode("ascii"),
        config_sha256.encode("ascii"),
    ):
        _framed(digest, value)
    return digest.hexdigest()


def _temporary_prefix(
    target: Path, *, pid: int | None = None, domain: str = _TEMPORARY_DOMAIN
) -> str:
    owner = os.getpid() if pid is None else pid
    return f".{target.name}.{domain}.{owner}."


def _temporary_owner_is_dead(candidate: Path) -> bool:
    match = _TEMPORARY_NAME.fullmatch(candidate.name)
    if match is None or match.group(1) != _TEMPORARY_DOMAIN:
        return False
    if os.name != "posix":
        return False
    try:
        owner = int(match.group(2), 10)
    except ValueError:
        return False
    try:
        os.kill(owner, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        return False
    except (OverflowError, ValueError):
        return False
    except OSError as error:
        if error.errno == errno.ESRCH:
            return True
        return False
    return False


@dataclass
class ProviderCacheStats:
    attempted_bytes: int = 0
    bytes: int = 0
    error_errno: int = 0
    errors: int = 0
    evictions: int = 0
    hits: int = 0
    misses: int = 0
    writes: int = 0
    invalid: int = 0
    no_space: int = 0
    skipped: int = 0
    temporaries_removed: int = 0

    def as_dict(self) -> Mapping[str, int]:
        return {
            "attempted_bytes": self.attempted_bytes,
            "bytes": self.bytes,
            "error_errno": self.error_errno,
            "errors": self.errors,
            "evictions": self.evictions,
            "hits": self.hits,
            "invalid": self.invalid,
            "misses": self.misses,
            "no_space": self.no_space,
            "skipped": self.skipped,
            "temporaries_removed": self.temporaries_removed,
            "writes": self.writes,
        }


@dataclass
class MapCacheStats:
    attempted_bytes: int = 0
    error_errno: int = 0
    errors: int = 0
    hits: int = 0
    invalid: int = 0
    misses: int = 0
    no_space: int = 0
    skipped: int = 0
    writes: int = 0

    def as_dict(self) -> Mapping[str, int]:
        return {
            "attempted_bytes": self.attempted_bytes,
            "error_errno": self.error_errno,
            "errors": self.errors,
            "hits": self.hits,
            "invalid": self.invalid,
            "misses": self.misses,
            "no_space": self.no_space,
            "skipped": self.skipped,
            "writes": self.writes,
        }


class _MapCacheWriter:
    """Collect one canonical Map without making cache failures observable."""

    def __init__(self, cache: "ProviderCache", target: Path) -> None:
        self._cache = cache
        self._target = target
        previous = cache._entries.get(target)
        self._previous_size = previous[0] if previous is not None else 0
        self._stream: BinaryIO | None = None
        self._temporary: Path | None = None
        self._size = 0
        try:
            target.parent.mkdir(parents=True, exist_ok=True)
            stream = tempfile.NamedTemporaryFile(
                dir=target.parent,
                prefix=_temporary_prefix(target),
                suffix=".tmp",
                delete=False,
            )
            self._stream = stream
            self._temporary = Path(stream.name)
        except OSError as error:
            self._record_error(error)

    def _record_error(self, error: OSError) -> None:
        self._cache.map_stats.errors += 1
        self._cache.map_stats.error_errno = error.errno or 0
        if error.errno in {errno.ENOSPC, getattr(errno, "EDQUOT", -1)}:
            self._cache.map_stats.no_space += 1

    def _discard(self) -> None:
        stream, self._stream = self._stream, None
        temporary, self._temporary = self._temporary, None
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
            except OSError:
                self._cache.map_stats.errors += 1

    def write(self, data: bytes) -> int:
        self._cache.map_stats.attempted_bytes = max(
            self._cache.map_stats.attempted_bytes,
            self._size + len(data),
        )
        stream = self._stream
        if stream is None:
            return len(data)
        if self._size + len(data) > self._cache.max_bytes:
            self._cache.map_stats.skipped += 1
            self._discard()
            return len(data)
        try:
            written = stream.write(data)
        except OSError as error:
            self._record_error(error)
            self._discard()
            return len(data)
        if written != len(data):
            self._record_error(OSError(errno.EIO, "short cache write"))
            self._discard()
            return len(data)
        self._size += written
        return written

    def abort(self) -> None:
        self._discard()

    def commit(self) -> None:
        stream = self._stream
        temporary = self._temporary
        if stream is None or temporary is None:
            return
        try:
            stream.flush()
            os.fsync(stream.fileno())
            stream.close()
            self._stream = None
            incoming = max(0, self._size - self._previous_size)
            self._cache._prune(incoming, preserve=self._target)
            if self._cache.stats.bytes + incoming > self._cache.max_bytes:
                self._cache.map_stats.skipped += 1
                self._discard()
                return
            os.replace(temporary, self._target)
            self._temporary = None
            metadata = self._target.stat()
            self._cache.stats.bytes += metadata.st_size - self._previous_size
            self._cache._entries[self._target] = (
                metadata.st_size,
                metadata.st_mtime_ns,
            )
            self._cache.map_stats.writes += 1
        except OSError as error:
            self._record_error(error)
            self._discard()


class ProviderCache:
    """Store raw canonical bundles; the native core still validates every hit."""

    def __init__(
        self, root: Path | str, *, max_bytes: int | None = None
    ) -> None:
        self.root = Path(root).expanduser()
        if max_bytes is None:
            self.max_bytes = default_provider_cache_max_bytes()
        elif (
            isinstance(max_bytes, bool)
            or not isinstance(max_bytes, int)
            or max_bytes <= 0
            or max_bytes > _MAX_SAFE_INTEGER
        ):
            raise ValueError(
                "provider cache max_bytes must be a positive safe integer"
            )
        else:
            self.max_bytes = max_bytes
        self.stats = ProviderCacheStats()
        self.map_stats = MapCacheStats()
        self._entries: dict[Path, tuple[int, int]] = {}
        self._inventory()

    def _inventory(self) -> None:
        for base in (self.root / "providers-v1", self.root / "maps-v1"):
            try:
                paths = tuple(base.rglob("*"))
            except OSError:
                self.stats.errors += 1
                continue
            for candidate in paths:
                try:
                    if candidate.name.startswith(
                        "."
                    ) and candidate.name.endswith(".tmp"):
                        if _temporary_owner_is_dead(candidate):
                            candidate.unlink()
                            self.stats.temporaries_removed += 1
                        continue
                    if (
                        candidate.suffix != ".json"
                        or candidate.is_symlink()
                        or not candidate.is_file()
                    ):
                        continue
                    metadata = candidate.stat()
                except FileNotFoundError:
                    continue
                except OSError:
                    self.stats.errors += 1
                    continue
                self._entries[candidate] = (
                    metadata.st_size,
                    metadata.st_mtime_ns,
                )
                self.stats.bytes += metadata.st_size
        self._prune(0)

    def _prune(self, incoming: int, *, preserve: Path | None = None) -> None:
        if self.stats.bytes + incoming <= self.max_bytes:
            return
        ordered = sorted(
            self._entries.items(),
            key=lambda row: (row[1][1], os.fsencode(str(row[0]))),
        )
        for candidate, (size, _) in ordered:
            if self.stats.bytes + incoming <= self.max_bytes:
                break
            if preserve is not None and candidate == preserve:
                continue
            try:
                candidate.unlink()
            except FileNotFoundError:
                pass
            except OSError:
                self.stats.errors += 1
                continue
            self._entries.pop(candidate, None)
            self.stats.bytes -= size
            self.stats.evictions += 1

    def _path(
        self,
        *,
        namespace: str,
        project: str,
        provider_id: str,
        path: str,
        source_sha256: str,
    ) -> Path:
        key = _cache_key(
            namespace=namespace,
            project=project,
            provider_id=provider_id,
            path=path,
            source_sha256=source_sha256,
        )
        return self.root / "providers-v1" / key[:2] / f"{key}.json"

    def _map_path(
        self,
        *,
        namespace: str,
        project: str,
        manifest_sha256: str,
        config_sha256: str,
    ) -> Path:
        key = _map_cache_key(
            namespace=namespace,
            project=project,
            manifest_sha256=manifest_sha256,
            config_sha256=config_sha256,
        )
        return self.root / "maps-v1" / key[:2] / f"{key}.json"

    def load(
        self,
        *,
        namespace: str,
        project: str,
        provider_id: str,
        path: str,
        source_sha256: str,
    ) -> bytes | None:
        target = self._path(
            namespace=namespace,
            project=project,
            provider_id=provider_id,
            path=path,
            source_sha256=source_sha256,
        )
        try:
            data = target.read_bytes()
        except FileNotFoundError:
            self.stats.misses += 1
            return None
        except OSError:
            self.stats.errors += 1
            self.stats.misses += 1
            return None
        self.stats.hits += 1
        return data

    def load_map(
        self,
        *,
        namespace: str,
        project: str,
        manifest_sha256: str,
        config_sha256: str,
    ) -> bytes | None:
        target = self._map_path(
            namespace=namespace,
            project=project,
            manifest_sha256=manifest_sha256,
            config_sha256=config_sha256,
        )
        try:
            data = target.read_bytes()
        except FileNotFoundError:
            self.map_stats.misses += 1
            return None
        except OSError:
            self.map_stats.errors += 1
            self.map_stats.misses += 1
            return None
        self.map_stats.hits += 1
        return data

    def store(
        self,
        data: bytes,
        *,
        namespace: str,
        project: str,
        provider_id: str,
        path: str,
        source_sha256: str,
    ) -> None:
        self.stats.attempted_bytes = max(self.stats.attempted_bytes, len(data))
        target = self._path(
            namespace=namespace,
            project=project,
            provider_id=provider_id,
            path=path,
            source_sha256=source_sha256,
        )
        if len(data) > self.max_bytes:
            self.stats.skipped += 1
            return
        previous = self._entries.get(target)
        previous_size = previous[0] if previous is not None else 0
        incoming = max(0, len(data) - previous_size)
        self._prune(incoming, preserve=target)
        if self.stats.bytes + incoming > self.max_bytes:
            self.stats.skipped += 1
            return
        temporary_name = ""
        try:
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                dir=target.parent,
                prefix=_temporary_prefix(target),
                suffix=".tmp",
                delete=False,
            ) as temporary:
                temporary.write(data)
                temporary.flush()
                os.fsync(temporary.fileno())
                temporary_name = temporary.name
            os.replace(temporary_name, target)
            metadata = target.stat()
            self.stats.bytes += metadata.st_size - previous_size
            self._entries[target] = (metadata.st_size, metadata.st_mtime_ns)
            self.stats.writes += 1
        except OSError as error:
            self.stats.errors += 1
            self.stats.error_errno = error.errno or 0
            if error.errno in {errno.ENOSPC, getattr(errno, "EDQUOT", -1)}:
                self.stats.no_space += 1
        finally:
            if temporary_name:
                try:
                    Path(temporary_name).unlink()
                except FileNotFoundError:
                    pass

    def store_map(
        self,
        data: bytes,
        *,
        namespace: str,
        project: str,
        manifest_sha256: str,
        config_sha256: str,
    ) -> None:
        if len(data) > self.max_bytes:
            self.map_stats.skipped += 1
            return
        writer = self.open_map_writer(
            namespace=namespace,
            project=project,
            manifest_sha256=manifest_sha256,
            config_sha256=config_sha256,
        )
        writer.write(data)
        writer.commit()

    def open_map_writer(
        self,
        *,
        namespace: str,
        project: str,
        manifest_sha256: str,
        config_sha256: str,
    ) -> _MapCacheWriter:
        """Open a bounded atomic sink for one compact canonical Map."""

        return _MapCacheWriter(
            self,
            self._map_path(
                namespace=namespace,
                project=project,
                manifest_sha256=manifest_sha256,
                config_sha256=config_sha256,
            ),
        )

    def reject(
        self,
        *,
        namespace: str,
        project: str,
        provider_id: str,
        path: str,
        source_sha256: str,
    ) -> None:
        """Discard a locally plausible bundle rejected by the native core."""

        target = self._path(
            namespace=namespace,
            project=project,
            provider_id=provider_id,
            path=path,
            source_sha256=source_sha256,
        )
        if self.stats.hits:
            self.stats.hits -= 1
        self.stats.invalid += 1
        self.stats.misses += 1
        try:
            target.unlink()
        except FileNotFoundError:
            pass
        except OSError:
            self.stats.errors += 1
            return
        previous = self._entries.pop(target, None)
        if previous is not None:
            self.stats.bytes -= previous[0]

    def reject_map(
        self,
        *,
        namespace: str,
        project: str,
        manifest_sha256: str,
        config_sha256: str,
    ) -> None:
        """Discard a complete Map that failed host-level identity validation."""

        target = self._map_path(
            namespace=namespace,
            project=project,
            manifest_sha256=manifest_sha256,
            config_sha256=config_sha256,
        )
        if self.map_stats.hits:
            self.map_stats.hits -= 1
        self.map_stats.invalid += 1
        self.map_stats.misses += 1
        try:
            target.unlink()
        except FileNotFoundError:
            pass
        except OSError:
            self.map_stats.errors += 1
            return
        previous = self._entries.pop(target, None)
        if previous is not None:
            self.stats.bytes -= previous[0]


__all__ = [
    "MapCacheStats",
    "ProviderCache",
    "ProviderCacheStats",
    "default_provider_cache_dir",
    "default_provider_cache_max_bytes",
]
