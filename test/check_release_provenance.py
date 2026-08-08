#!/usr/bin/env python3
"""Bind release archives to one exact source commit and release tag."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
from typing import Iterator
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.csrc_bundle import decode_bundle


def _git(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode:
        raise ValueError(
            f"git {' '.join(arguments)} failed: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def _git_bytes(repository: Path, revision: str, path: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repository), "show", f"{revision}:{path}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise ValueError(
            f"git show {revision}:{path} failed: "
            f"{result.stderr.decode('utf-8', errors='replace').strip()}"
        )
    return result.stdout


def _members(path: Path) -> Iterator[tuple[str, bytes]]:
    if path.suffix == ".whl":
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    yield info.filename, archive.read(info)
        return
    with tarfile.open(path, "r:*") as archive:
        for info in archive.getmembers():
            if not info.isfile():
                continue
            source = archive.extractfile(info)
            if source is None:
                raise ValueError(f"cannot read archive member: {info.name}")
            yield info.name, source.read()


def _one_member(
    members: dict[str, bytes], predicate, label: str
) -> tuple[str, bytes] | None:
    selected = sorted(
        (name, data) for name, data in members.items() if predicate(name)
    )
    if len(selected) != 1:
        return None
    return selected[0]


def _release_document(
    path: Path, members: dict[str, bytes]
) -> tuple[str, dict[str, object]] | None:
    if path.suffix == ".whl":
        selected = _one_member(
            members,
            lambda name: PurePosixPath(name).parts == ("archbird", "release.json"),
            "Python wheel release provenance",
        )
    elif path.name.endswith(".tgz"):
        selected = _one_member(
            members,
            lambda name: PurePosixPath(name).parts == ("package", "release.json"),
            "npm release provenance",
        )
    else:
        selected = _one_member(
            members,
            lambda name: len(PurePosixPath(name).parts) == 3
            and PurePosixPath(name).parts[1:] == ("archbird", "release.json"),
            "Python sdist release provenance",
        )
    if selected is None:
        return None
    name, data = selected
    try:
        document = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return name, {}
    return name, document


def _manifest_digest(
    members: dict[str, bytes], directory: str, *, required: bool
) -> str | None:
    selected = _one_member(
        members,
        lambda name: (
            PurePosixPath(name).name == ".archbird-manifest.json"
            and directory in PurePosixPath(name).parts
        ),
        f"{directory} manifest",
    )
    if selected is None:
        if directory == "csrc":
            bundle = _one_member(
                members,
                lambda name: PurePosixPath(name).name == "csrc.snapshot.gz",
                "compressed C source snapshot",
            )
            if bundle is not None:
                try:
                    manifest = decode_bundle(bundle[1])[".archbird-manifest.json"]
                except (KeyError, ValueError) as error:
                    raise ValueError(
                        "archive contains an invalid compressed C source snapshot"
                    ) from error
                return hashlib.sha256(manifest).hexdigest()
        if required:
            raise ValueError(f"archive omitted its {directory} content manifest")
        return None
    return hashlib.sha256(selected[1]).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-commit")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--require-tag", action="store_true")
    parser.add_argument("--output")
    parser.add_argument("archives", nargs="+")
    args = parser.parse_args()

    repository = Path(args.repository).resolve()
    source_commit = args.source_commit or _git(repository, "rev-parse", "HEAD")
    if _git(repository, "rev-parse", f"{source_commit}^{{commit}}") != source_commit:
        raise AssertionError("source commit is not a full canonical hash")
    tag = f"v{args.version}"
    source_tree = _git(repository, "rev-parse", f"{source_commit}^{{tree}}")
    python_readme = _git_bytes(repository, source_commit, "py/README.md")
    node_readme = _git_bytes(repository, source_commit, "js/README.md")
    node_package = json.loads(
        _git_bytes(repository, source_commit, "js/package.json")
    )
    failures: list[str] = []

    if args.require_clean:
        dirty = _git(repository, "status", "--porcelain", "--untracked-files=no")
        if dirty:
            failures.append("tracked worktree is not clean")

    try:
        tag_commit = _git(repository, "rev-parse", f"{tag}^{{commit}}")
    except ValueError as error:
        tag_commit = ""
        failures.append(str(error))
    if args.require_tag and tag_commit != source_commit:
        failures.append(
            f"release tag {tag} points to {tag_commit or 'nothing'}, "
            f"expected {source_commit}"
        )

    archive_rows: list[dict[str, object]] = []
    archive_kinds: list[str] = []
    for raw_path in args.archives:
        path = Path(raw_path).resolve()
        data = path.read_bytes()
        members = dict(_members(path))
        release = _release_document(path, members)
        row: dict[str, object] = {
            "bytes": len(data),
            "path": str(path),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        if release is None:
            failures.append(f"{path.name}: release provenance is absent")
        else:
            member_name, document = release
            row["release_provenance"] = member_name
            row["release_provenance_sha256"] = hashlib.sha256(
                members[member_name]
            ).hexdigest()
            row["source_commit"] = document.get("source_commit")
            if document.get("artifact") != "archbird-release-source":
                failures.append(f"{path.name}: invalid release provenance artifact")
            if document.get("schema_version") != 1:
                failures.append(
                    f"{path.name}: invalid release provenance schema version"
                )
            if document.get("version") != args.version:
                failures.append(f"{path.name}: release provenance version differs")
            if document.get("source_commit") != source_commit:
                failures.append(f"{path.name}: release provenance commit differs")
            if document.get("source_tree") != source_tree:
                failures.append(f"{path.name}: release provenance tree differs")
            if document.get("tag") != tag:
                failures.append(f"{path.name}: release provenance tag differs")

        if path.suffix == ".whl":
            archive_kinds.append("python-wheel")
            row["schema_manifest_sha256"] = _manifest_digest(
                members, "schemas", required=True
            )
            metadata = _one_member(
                members,
                lambda name: name.endswith(".dist-info/METADATA"),
                "Python wheel metadata",
            )
            if metadata is None or b"\n\n" not in metadata[1]:
                failures.append(f"{path.name}: wheel metadata is absent")
            else:
                row["package_metadata_sha256"] = hashlib.sha256(
                    metadata[1]
                ).hexdigest()
                packaged_readme = metadata[1].split(b"\n\n", 1)[1]
                row["readme_sha256"] = hashlib.sha256(
                    packaged_readme
                ).hexdigest()
                if packaged_readme != python_readme:
                    failures.append(
                        f"{path.name}: packaged Python README differs from "
                        "source commit"
                    )
        elif path.name.endswith(".tgz"):
            archive_kinds.append("npm-package")
            row["c_source_manifest_sha256"] = _manifest_digest(
                members, "csrc", required=True
            )
            row["schema_manifest_sha256"] = _manifest_digest(
                members, "schema", required=True
            )
            package = _one_member(
                members,
                lambda name: PurePosixPath(name).parts == (
                    "package",
                    "package.json",
                ),
                "npm package metadata",
            )
            if package is None:
                failures.append(f"{path.name}: package.json is absent")
            else:
                row["package_metadata_sha256"] = hashlib.sha256(
                    package[1]
                ).hexdigest()
                try:
                    metadata = json.loads(package[1])
                except (UnicodeDecodeError, json.JSONDecodeError):
                    metadata = {}
                row["gitHead"] = metadata.get("gitHead")
                if metadata.get("gitHead") != source_commit:
                    failures.append(f"{path.name}: npm gitHead differs or is absent")
                without_git_head = dict(metadata)
                without_git_head.pop("gitHead", None)
                if without_git_head != node_package:
                    failures.append(
                        f"{path.name}: npm package metadata differs from source commit"
                    )
            readme = members.get("package/README.md")
            if readme is not None:
                row["readme_sha256"] = hashlib.sha256(readme).hexdigest()
            if readme != node_readme:
                failures.append(
                    f"{path.name}: packaged npm README differs from source commit"
                )
        else:
            archive_kinds.append("python-sdist")
            row["c_source_manifest_sha256"] = _manifest_digest(
                members, "csrc", required=True
            )
            row["schema_manifest_sha256"] = _manifest_digest(
                members, "schemas", required=True
            )
            roots = {
                PurePosixPath(name).parts[0]
                for name in members
                if PurePosixPath(name).parts
            }
            root = next(iter(roots)) if len(roots) == 1 else ""
            readme = members.get(f"{root}/README.md") if root else None
            if readme is not None:
                row["readme_sha256"] = hashlib.sha256(readme).hexdigest()
            if readme != python_readme:
                failures.append(
                    f"{path.name}: packaged Python README differs from source commit"
                )
        archive_rows.append(row)

    expected_archive_kinds = ["npm-package", "python-sdist", "python-wheel"]
    if sorted(archive_kinds) != expected_archive_kinds:
        failures.append(
            "release archive inventory differs: "
            f"expected={expected_archive_kinds!r} "
            f"actual={sorted(archive_kinds)!r}"
        )

    report = {
        "artifact": "archbird-release-provenance-check",
        "archives": archive_rows,
        "failures": failures,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "tag": tag,
        "tag_commit": tag_commit,
        "version": args.version,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(encoded, encoding="utf-8")
    if failures:
        raise AssertionError(
            "release provenance check failed:\n- " + "\n- ".join(failures)
        )
    print(
        f"release provenance passed: version={args.version} "
        f"commit={source_commit} archives={len(archive_rows)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
