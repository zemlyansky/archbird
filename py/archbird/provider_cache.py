"""Persistent content-addressed cache for normalized provider-facts bundles."""

from __future__ import annotations

from dataclasses import dataclass
import errno
import hashlib
import os
from pathlib import Path
import tempfile
from typing import Mapping


_CACHE_CONTRACT = b"archbird-provider-cache-v1"
_DEFAULT_MAX_BYTES = 1024 * 1024 * 1024
_MAX_SAFE_INTEGER = (1 << 53) - 1


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


@dataclass
class ProviderCacheStats:
    bytes: int = 0
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
            "bytes": self.bytes,
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
        self._entries: dict[Path, tuple[int, int]] = {}
        self._inventory()

    def _inventory(self) -> None:
        base = self.root / "providers-v1"
        try:
            paths = tuple(base.rglob("*"))
        except OSError:
            self.stats.errors += 1
            return
        for candidate in paths:
            try:
                if candidate.name.startswith(".") and candidate.name.endswith(
                    ".tmp"
                ):
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
            self._entries[candidate] = (metadata.st_size, metadata.st_mtime_ns)
            self.stats.bytes += metadata.st_size
        self._prune(0)

    def _prune(self, incoming: int, *, preserve: Path | None = None) -> None:
        if incoming > self.max_bytes:
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
                prefix=f".{target.stem}.",
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
            if error.errno in {errno.ENOSPC, getattr(errno, "EDQUOT", -1)}:
                self.stats.no_space += 1
        finally:
            if temporary_name:
                try:
                    Path(temporary_name).unlink()
                except FileNotFoundError:
                    pass

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
        else:
            previous = self._entries.pop(target, None)
            if previous is not None:
                self.stats.bytes -= previous[0]


__all__ = [
    "ProviderCache",
    "ProviderCacheStats",
    "default_provider_cache_dir",
    "default_provider_cache_max_bytes",
]
