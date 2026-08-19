#!/usr/bin/env python3
"""Check a reviewed Archbird self-Map transition against Git and Verify."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


UNCHANGED_PUBLIC_SECTIONS = (
    "entrypoints",
    "package_dependencies",
    "package_entrypoint_surfaces",
    "package_export_origins",
    "package_exports",
    "parity_gaps",
    "public_symbols",
)

VALID_POLICIES = ("repair", "feature")


def _load(path: Path) -> tuple[bytes, dict[str, object]]:
    data = path.read_bytes()
    document = json.loads(data)
    if not isinstance(document, dict):
        raise AssertionError(f"artifact is not a JSON object: {path}")
    return data, document


def _git(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode:
        raise AssertionError(
            f"git {' '.join(arguments)} failed: {result.stderr.strip()}"
        )
    return result.stdout.strip()


def _map_changed_files(diff: dict[str, object]) -> set[str]:
    sections = diff.get("sections")
    if not isinstance(sections, dict):
        raise AssertionError("Diff omitted sections")
    files = sections.get("files")
    if not isinstance(files, dict):
        raise AssertionError("Diff omitted files section")
    changed = set(files.get("added", [])) | set(files.get("removed", []))
    for value in files.get("changed", []):
        if not isinstance(value, str) or ": " not in value:
            raise AssertionError("Diff contains an invalid changed-file record")
        changed.add(value.split(": ", 1)[0])
    if not all(isinstance(value, str) for value in changed):
        raise AssertionError("Diff contains a non-string file path")
    return changed


def _git_changed_files(repository: Path, before: str) -> set[str]:
    tracked = set(
        _git(repository, "diff", "--name-only", before, "--").splitlines()
    )
    untracked = {
        path
        for path in _git(
            repository, "ls-files", "--others", "--exclude-standard"
        ).splitlines()
        if path != ".claude" and not path.startswith(".claude/")
    }
    return tracked | untracked


def _section_has_changes(diff: dict[str, object], name: str) -> bool:
    sections = diff["sections"]
    section = sections.get(name)
    if not isinstance(section, dict):
        raise AssertionError(f"Diff omitted {name} section")
    return any(section.get(kind) for kind in ("added", "changed", "removed"))


def _freshness_failures(
    label: str,
    freshness: dict[str, object],
    mapped: dict[str, object],
) -> list[str]:
    evidence = mapped.get("evidence")
    snapshot = freshness.get("snapshot")
    current = freshness.get("current")
    failures: list[str] = []
    if (
        freshness.get("artifact") != "map-freshness"
        or freshness.get("status") != "current"
        or not isinstance(evidence, dict)
        or not isinstance(snapshot, dict)
        or not isinstance(current, dict)
        or snapshot.get("artifact") != "map"
        or snapshot.get("input_sha256") != evidence.get("input_sha256")
        or current.get("input_sha256") != evidence.get("input_sha256")
    ):
        failures.append(f"{label} Map freshness is not current and identity-bound")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--before-ref", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--before-map", required=True)
    parser.add_argument("--after-map", required=True)
    parser.add_argument("--diff", required=True)
    parser.add_argument("--before-freshness", required=True)
    parser.add_argument("--after-freshness", required=True)
    parser.add_argument("--verification", required=True)
    parser.add_argument("--expected-changes", required=True)
    parser.add_argument("--allow-unmapped", action="append", default=[])
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--require-tag", action="store_true")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    repository = Path(args.repository).resolve()
    before_path = Path(args.before_map).resolve()
    after_path = Path(args.after_map).resolve()
    diff_path = Path(args.diff).resolve()
    before_freshness_path = Path(args.before_freshness).resolve()
    after_freshness_path = Path(args.after_freshness).resolve()
    verification_path = Path(args.verification).resolve()
    expected_changes_path = Path(args.expected_changes).resolve()
    before_data, before_map = _load(before_path)
    after_data, after_map = _load(after_path)
    diff_data, diff = _load(diff_path)
    before_freshness_data, before_freshness = _load(before_freshness_path)
    after_freshness_data, after_freshness = _load(after_freshness_path)
    verification_data, verification = _load(verification_path)
    expected_changes_data, expected_changes = _load(expected_changes_path)
    failures: list[str] = []

    if before_map.get("artifact") != "map" or after_map.get("artifact") != "map":
        failures.append("self-transition inputs are not canonical Maps")
    if diff.get("artifact") != "diff":
        failures.append("self-transition comparison is not a Diff")
    if verification.get("artifact") != "verification":
        failures.append("self-transition policy result is not Verification")
    failures.extend(_freshness_failures("baseline", before_freshness, before_map))
    failures.extend(_freshness_failures("candidate", after_freshness, after_map))

    before_tool = before_map.get("tool")
    after_tool = after_map.get("tool")
    if (
        not isinstance(before_tool, dict)
        or before_tool != after_tool
        or before_tool.get("version") != args.version
    ):
        failures.append("before and after Maps did not use one release engine")
    if diff.get("tool") != after_tool:
        failures.append("Diff engine differs from self Maps")
    if verification.get("tool") != after_tool:
        failures.append("Verification engine differs from self Maps")

    for side, mapped in (("before", before_map), ("after", after_map)):
        diff_side = diff.get(side)
        evidence = mapped.get("evidence")
        if (
            not isinstance(diff_side, dict)
            or not isinstance(evidence, dict)
            or diff_side.get("input_sha256") != evidence.get("input_sha256")
            or diff_side.get("project") != mapped.get("project")
        ):
            failures.append(f"Diff {side} identity differs from its Map")
        diagnostics = mapped.get("diagnostics")
        if not isinstance(diagnostics, list) or any(
            isinstance(row, dict) and row.get("severity") == "error"
            for row in diagnostics
        ):
            failures.append(f"{side} Map contains blocking diagnostics")

    summary = verification.get("summary")
    constraint_summary = (
        summary.get("constraints") if isinstance(summary, dict) else None
    )
    if (
        not isinstance(summary, dict)
        or summary.get("blocking") is not False
        or not isinstance(constraint_summary, dict)
        or constraint_summary.get("fail") != 0
        or constraint_summary.get("unknown") != 0
    ):
        failures.append("candidate self Verification did not pass exhaustively")

    before_commit = _git(repository, "rev-parse", f"{args.before_ref}^{{commit}}")
    source_commit = _git(repository, "rev-parse", "HEAD^{commit}")
    source_tree = _git(repository, "rev-parse", "HEAD^{tree}")
    tag = f"v{args.version}"
    if args.require_clean:
        dirty = _git(repository, "status", "--porcelain", "--untracked-files=no")
        if dirty:
            failures.append("tracked worktree is not clean")
        unexpected_untracked = {
            path
            for path in _git(
                repository, "ls-files", "--others", "--exclude-standard"
            ).splitlines()
            if path != ".claude" and not path.startswith(".claude/")
        }
        if unexpected_untracked:
            failures.append(
                "unexpected untracked release paths: "
                + ", ".join(sorted(unexpected_untracked))
            )
    if args.require_tag:
        try:
            tag_commit = _git(repository, "rev-parse", f"{tag}^{{commit}}")
        except AssertionError:
            tag_commit = ""
        if tag_commit != source_commit:
            failures.append(
                f"release tag {tag} does not identify source commit"
            )

    git_paths = _git_changed_files(repository, args.before_ref)
    expected_paths = expected_changes.get("paths")
    policy = expected_changes.get("policy", "repair")
    allow_public_changes = expected_changes.get("allow_public_changes", [])
    if (
        expected_changes.get("artifact") != "archbird-release-change-envelope"
        or expected_changes.get("before_ref") != args.before_ref
        or expected_changes.get("version") != args.version
        or policy not in VALID_POLICIES
        or not isinstance(allow_public_changes, list)
        or allow_public_changes != sorted(set(allow_public_changes))
        or not all(
            isinstance(name, str) and name in UNCHANGED_PUBLIC_SECTIONS
            for name in allow_public_changes
        )
        or (policy == "repair" and allow_public_changes)
        or not isinstance(expected_paths, list)
        or expected_paths != sorted(set(expected_paths))
        or not all(isinstance(path, str) and path for path in expected_paths)
    ):
        failures.append("reviewed release change envelope is invalid")
    elif git_paths != set(expected_paths):
        failures.append(
            "Git transition differs from reviewed change envelope: "
            f"unexpected={sorted(git_paths - set(expected_paths))!r} "
            f"missing={sorted(set(expected_paths) - git_paths)!r}"
        )
    map_paths = _map_changed_files(diff)
    allowed_unmapped = set(args.allow_unmapped)
    unexpected_map = map_paths - git_paths
    missed_git = git_paths - map_paths - allowed_unmapped
    unused_allowance = allowed_unmapped - (git_paths - map_paths)
    if unexpected_map:
        failures.append(
            "Map Diff contains paths absent from Git transition: "
            + ", ".join(sorted(unexpected_map))
        )
    if missed_git:
        failures.append(
            "Git transition contains paths absent from Map Diff: "
            + ", ".join(sorted(missed_git))
        )
    if unused_allowance:
        failures.append(
            "unused unmapped-path allowance: "
            + ", ".join(sorted(unused_allowance))
        )
    files = diff["sections"]["files"]
    if policy == "repair" and files.get("removed"):
        failures.append("repair release unexpectedly removes mapped files")
    approved_public = set(allow_public_changes) if policy == "feature" else set()
    for name in UNCHANGED_PUBLIC_SECTIONS:
        if _section_has_changes(diff, name) and name not in approved_public:
            failures.append(f"{policy} release unexpectedly changes {name}")

    report = {
        "after_map": {
            "input_sha256": after_map.get("evidence", {}).get("input_sha256"),
            "sha256": hashlib.sha256(after_data).hexdigest(),
        },
        "artifact": "archbird-release-transition",
        "after_freshness": {
            "sha256": hashlib.sha256(after_freshness_data).hexdigest(),
            "status": after_freshness.get("status"),
        },
        "before_commit": before_commit,
        "before_map": {
            "input_sha256": before_map.get("evidence", {}).get("input_sha256"),
            "sha256": hashlib.sha256(before_data).hexdigest(),
        },
        "before_ref": args.before_ref,
        "before_freshness": {
            "sha256": hashlib.sha256(before_freshness_data).hexdigest(),
            "status": before_freshness.get("status"),
        },
        "changed_paths": sorted(map_paths),
        "diff_sha256": hashlib.sha256(diff_data).hexdigest(),
        "expected_changes_sha256": hashlib.sha256(
            expected_changes_data
        ).hexdigest(),
        "failures": failures,
        "schema_version": 1,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "tag": tag,
        "verification_result_sha256": verification.get(
            "verification_result_sha256"
        ),
        "verification_sha256": hashlib.sha256(verification_data).hexdigest(),
        "version": args.version,
        "policy": policy,
        "allow_public_changes": list(allow_public_changes),
    }
    output = Path(args.output)
    output.write_text(
        json.dumps(report, ensure_ascii=True, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if failures:
        raise AssertionError(
            "release self-transition check failed:\n- "
            + "\n- ".join(failures)
        )
    print(
        f"release self-transition passed: {args.before_ref} -> "
        f"{source_commit[:12]} paths={len(map_paths)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
