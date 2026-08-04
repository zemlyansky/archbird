#!/usr/bin/env python3
"""Replay the fixed open Path development/validation evaluation track."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import sys


def load_extension(path: Path) -> None:
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)


if len(sys.argv) > 1:
    load_extension(Path(sys.argv[1]).resolve())

from archbird.native import Project  # noqa: E402


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    track_path = repository / "test/fixtures/path_evaluation.json"
    track = json.loads(track_path.read_text(encoding="utf-8"))
    if (
        track.get("artifact") != "archbird-path-evaluation-track"
        or track.get("schema_version") != 1
        or track.get("provenance") != "asserted"
        or track.get("held_out_policy") != "sealed_external"
    ):
        raise AssertionError("Path evaluation track identity is invalid")
    cases = track.get("cases")
    subjects = track.get("subjects")
    if not isinstance(cases, list) or not cases or not isinstance(subjects, dict):
        raise AssertionError("Path evaluation track is empty")
    if any(case.get("split") not in {"development", "validation"} for case in cases):
        raise AssertionError("open Path track contains a held-out case")
    ids = [case.get("id") for case in cases]
    if ids != list(dict.fromkeys(ids)):
        raise AssertionError("Path evaluation case identities are not unique")

    root = repository / "build/path-evaluation"
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    projects: dict[tuple[str, bytes], Project] = {}
    results: dict[str, int] = {"development": 0, "validation": 0}
    try:
        for case in cases:
            subject_id = case["subject"]
            subject = subjects[subject_id]
            config = case.get("config", subject.get("config"))
            config_json = canonical(config) if config is not None else b""
            project_key = (subject_id, config_json)
            project = projects.get(project_key)
            if project is None:
                subject_root = root / f"{subject_id}-{len(projects)}"
                for relative, source in subject["sources"].items():
                    path = subject_root / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(source, encoding="utf-8")
                project = Project.from_repository(
                    subject_root,
                    config=config_json or None,
                    project=f"path-evaluation-{subject_id}",
                    ignore=False,
                    jobs=1,
                    cache_dir=None,
                    map_cache=False,
                )
                projects[project_key] = project

            request = case["request"]
            result = project.path(
                request["source"],
                request["target"],
                level=request.get("level", "file"),
                relations=request.get("relations"),
                direction=request.get("direction", "downstream"),
                max_depth=request.get("max_depth", 8),
                max_paths=request.get("max_paths", 8),
            )
            expected = case["expected"]
            actual_nodes = [path["nodes"] for path in result["paths"]]
            actual_states = {path["state"] for path in result["paths"]}
            expected_state = expected["path_state"]
            if (
                result["outcome"] != expected["outcome"]
                or result["reason"] != expected["reason"]
                or result["search"]["shortest_length"]
                != expected["shortest_length"]
                or actual_nodes != expected["nodes"]
                or (
                    expected_state is None
                    and actual_states
                )
                or (
                    expected_state is not None
                    and actual_states != {expected_state}
                )
            ):
                raise AssertionError(
                    f"Path evaluation case {case['id']} changed: "
                    f"{result!r}"
                )
            if result["outcome"] == "found" and expected_state != "current":
                raise AssertionError(
                    f"Path evaluation case {case['id']} proves weak evidence"
                )
            results[case["split"]] += 1
    finally:
        shutil.rmtree(root)

    if results != {"development": 3, "validation": 5}:
        raise AssertionError(f"Path evaluation split changed: {results!r}")
    print(
        "fixed Path evaluation passed "
        f"({results['development']} development, "
        f"{results['validation']} validation; held-out sealed)"
    )


if __name__ == "__main__":
    main()
