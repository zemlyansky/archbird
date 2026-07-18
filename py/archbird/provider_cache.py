"""Persistent content-addressed cache for normalized provider-facts bundles."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import tempfile
from typing import Mapping


_CACHE_CONTRACT = b"archbird-provider-cache-v1"


def default_provider_cache_dir() -> Path:
    """Return the host cache root without binding artifacts to absolute paths."""

    configured = os.environ.get("ARCHBIRD_CACHE_DIR")
    if configured:
        return Path(configured).expanduser()
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return Path(xdg).expanduser() / "archbird"
    return Path.home() / ".cache" / "archbird"


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
    errors: int = 0
    hits: int = 0
    misses: int = 0
    writes: int = 0
    invalid: int = 0

    def as_dict(self) -> Mapping[str, int]:
        return {
            "errors": self.errors,
            "hits": self.hits,
            "invalid": self.invalid,
            "misses": self.misses,
            "writes": self.writes,
        }


class ProviderCache:
    """Store raw canonical bundles; the native core still validates every hit."""

    def __init__(self, root: Path | str) -> None:
        self.root = Path(root).expanduser()
        self.stats = ProviderCacheStats()

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
            self.stats.writes += 1
        except OSError:
            self.stats.errors += 1
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


__all__ = [
    "ProviderCache",
    "ProviderCacheStats",
    "default_provider_cache_dir",
]
