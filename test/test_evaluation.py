#!/usr/bin/env python3
"""Exercise external evaluation freezing, replay, and prior-run comparison."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


REPOSITORY = Path(__file__).resolve().parents[1]
TEMPORARY = REPOSITORY / "build/evaluation-test"
EVALUATOR = REPOSITORY / "tools/evaluate.py"


def run(*arguments: str, environment: dict[str, str], check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(EVALUATOR), *arguments],
        cwd=REPOSITORY,
        env=environment,
        check=check,
        capture_output=True,
        text=True,
    )


def git(root: Path, *arguments: str, environment: dict[str, str] | None = None) -> str:
    return subprocess.run(
        ["git", *arguments],
        cwd=root,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    shutil.rmtree(TEMPORARY, ignore_errors=True)
    source = TEMPORARY / "source"
    root = TEMPORARY / "external"
    source.mkdir(parents=True)
    git(source, "init", "--initial-branch=main")
    git(source, "config", "user.name", "Archbird Test")
    git(source, "config", "user.email", "archbird@example.invalid")
    (source / "src").mkdir()
    (source / "tests").mkdir()
    (source / "src/a.py").write_text(
        "def target():\n    return 1\n\ndef helper():\n    return target()\n",
        encoding="utf-8",
    )
    (source / "src/other.py").write_text("def other():\n    return 0\n", encoding="utf-8")
    (source / "tests/test_a.py").write_text("def test_target():\n    assert True\n", encoding="utf-8")
    git(source, "add", ".")
    git(source, "commit", "-m", "initial")
    before = git(source, "rev-parse", "HEAD")
    (source / "src/a.py").write_text(
        "def target():\n    return 2\n\ndef helper():\n    return target()\n",
        encoding="utf-8",
    )
    git(source, "add", "src/a.py")
    git(source, "commit", "-m", "change target")
    after = git(source, "rev-parse", "HEAD")

    fake = TEMPORARY / "fake-archbird.py"
    fake.write_text(
        """#!/usr/bin/env python3
import json, os, pathlib, sys
if sys.argv[1:] == ['--version']:
  print('0.test')
  raise SystemExit(0)
out = pathlib.Path(sys.argv[sys.argv.index('--output') + 1])
out.parent.mkdir(parents=True, exist_ok=True)
if sys.argv[1] == 'map':
  value = {'artifact':'map','schema_version':7,'project':'fixture','files':[],
           'source_tool':{'name':'archbird','version':'0.test',
             'implementation_sha256':'1'*64}}
else:
  variant = os.environ.get('ARCHBIRD_EVAL_TEST_VARIANT')
  good = variant in ('good', 'context')
  files = ([{'distance':0,'path':'src/a.py','symbols':[{'name':'target'}]}]
           if good else [{'distance':0,'path':'src/other.py','symbols':[{'name':'other'}]}])
  if variant == 'context':
    files[0]['symbols'].append({'name':'direct_caller'})
  matches = ([{'path':'tests/test_a.py','selector':'test_target'}] if good else [])
  matched = ([{'path':'src/a.py','name':'target'}] if good else [])
  value = {'artifact':'query','schema_version':7,'files':files,
           'matched_symbols':matched,'test_matches':matches}
out.write_text(json.dumps(value, separators=(',', ':'), sort_keys=True) + '\\n')
""",
        encoding="utf-8",
    )
    fake.chmod(0o755)

    environment = os.environ.copy()
    environment["ARCHBIRD_EVAL_ROOT"] = str(root)
    missing = os.environ.copy()
    missing.pop("ARCHBIRD_EVAL_ROOT", None)
    assert run("init", environment=missing, check=False).returncode == 2
    run("init", environment=environment)
    reservation_plan = {
        "artifact": "archbird-evaluation-reservation-plan",
        "provenance": "asserted",
        "repositories": [
            {
                "id": "reserved-fixture",
                "inspection_policy": "open_after_reservation",
                "primary_language": "python",
                "rationale": "Synthetic validation reservation.",
                "split": "validation",
                "url": str(source),
            }
        ],
        "schema_version": 1,
        "selection_rule": "Select before inspecting any post-reservation history.",
    }
    write_json(root / "authoring/reservations.json", reservation_plan)
    protocol = {
        "analysis": {
            "configuration": "zero_config",
            "depth": 1,
            "selector_policy": "task_explicit_identity",
            "test_depth": 1,
            "track": "seeded_routing",
        },
        "artifact": "archbird-evaluation-protocol",
        "case_selection": {
            "excluded_subject_prefixes": ["Bump ", "docs:"],
            "max_changed_files": 8,
            "order": "reverse_chronological_first_eligible",
            "require_product_source": True,
            "require_tests": True,
            "single_parent": True,
        },
        "id": "synthetic-seeded-routing-v1",
        "metrics": {
            "context": ["distractor_ratio", "returned_count"],
            "introduced_tests_role": "act_only",
            "performance": "observational_single_sample",
            "retrieval": ["mrr", "recall_all", "recall_at_20"],
            "split_aggregation": "separate",
        },
        "provenance": "asserted",
        "schema_version": 1,
        "split_policy": {
            "development": "tuning_allowed",
            "held_out": "sealed_until_protocol_frozen",
            "held_out_open_once": True,
            "tuning_after_held_out_open": False,
            "validation": "open_after_reservation",
        },
    }
    write_json(root / "authoring/protocol.json", protocol)
    run("reserve", environment=environment)
    state = json.loads((root / "state.json").read_text())
    first_reservation = state["current_reservation_sha256"]
    reservation = json.loads(
        (root / f"reservations/{first_reservation}/reservation.json").read_text()
    )
    assert reservation["repositories"][0]["default_branch"] == "main"
    assert reservation["repositories"][0]["head_revision"] == after
    case = {
        "analysis": {
            "depth": 1,
            "seed_rationale": "The task explicitly names target.",
            "selectors": [{"kind": "symbol", "value": "src/a.py:target"}],
            "test_depth": 1,
            "track": "exact_impact",
        },
        "artifact": "archbird-evaluation-case",
        "ground_truth": {
            "architecture_obligations": [],
            "introduced_tests": [],
            "notes": "Synthetic exact-impact acceptance case.",
            "relevant_files": ["src/a.py", "tests/test_a.py"],
            "relevant_symbols": ["src/a.py:helper", "src/a.py:target"],
            "relevant_tests": ["tests/test_a.py::test_target"],
        },
        "id": "fixture-target-change",
        "provenance": "asserted",
        "repository": {
            "after_revision": after,
            "before_revision": before,
            "id": "fixture",
            "url": str(source),
        },
        "review": {
            "evidence": ["git:single-parent", "test:synthetic-route-reviewed"],
            "rationale": "The synthetic change and exact relevance set are fully inspected.",
            "reviewer": "archbird-synthetic-gate",
            "status": "reviewed",
        },
        "schema_version": 1,
        "split": "development",
        "task": {
            "description": "Change the exact target and retain its test route.",
            "source": "fixture:change-target",
            "title": "Change target",
        },
    }
    case_path = root / "authoring/cases/fixture-target-change.json"
    write_json(case_path, case)
    run("sync", "--fetch", "--reservations", "validation", environment=environment)
    run("validate", environment=environment)
    run("freeze", environment=environment)
    state = json.loads((root / "state.json").read_text())
    first_corpus = state["current_corpus_sha256"]
    corpus = json.loads((root / f"corpora/{first_corpus}/corpus.json").read_text())
    assert corpus["cases"][0]["diff_files"] == ["src/a.py"]

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "bad"
    run("run", "--archbird", str(fake), "--label", "bad", environment=environment)
    first_run = json.loads((root / "state.json").read_text())["current_run_sha256"]
    first = json.loads((root / f"runs/{first_run}/result.json").read_text())
    assert first["cases"][0]["metrics"]["relevant_file_recall_all"] == 0.0

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "good"
    run("run", "--archbird", str(fake), "--label", "good", environment=environment)
    state = json.loads((root / "state.json").read_text())
    second_run = state["current_run_sha256"]
    assert state["previous_run_sha256"] == first_run
    comparison_path = root / state["current_run_comparison"]
    comparison = json.loads(comparison_path.read_text())
    row = comparison["cases"][0]
    assert row["metrics"]["relevant_file_recall_all"]["classification"] == "improved"
    assert row["metrics"]["relevant_test_recall_all"]["classification"] == "improved"
    assert comparison["summary"]["reviewed_quality"]["improved"] == 1
    second = json.loads((root / f"runs/{second_run}/result.json").read_text())
    assert second["aggregate"]["reviewed_by_split"]["development"]["cases"] == 1
    assert second["aggregate"]["reviewed_by_split"]["held_out"]["cases"] == 0
    assert second["aggregate"]["reviewed_by_split"]["development"][
        "metrics_pooled"
    ]["relevant_file"] == {
        "hits_all": 1,
        "hits_at_5": 1,
        "hits_at_10": 1,
        "hits_at_20": 1,
        "recall_all": 0.5,
        "recall_at_5": 0.5,
        "recall_at_10": 0.5,
        "recall_at_20": 0.5,
        "returned": 1,
        "truth": 2,
    }
    assert second["aggregate"]["reviewed_by_split"]["development"][
        "metrics_pooled"
    ]["relevant_context_file"] == {
        "hits_all": 2,
        "hits_at_5": 2,
        "hits_at_10": 2,
        "hits_at_20": 2,
        "recall_all": 1.0,
        "recall_at_5": 1.0,
        "recall_at_10": 1.0,
        "recall_at_20": 1.0,
        "returned": 2,
        "truth": 2,
    }
    assert second["aggregate"]["reviewed_by_split"]["development"][
        "metrics_pooled"
    ]["relevant_symbol"]["recall_all"] == 0.5
    assert second["cases"][0]["metrics"]["matched_symbol_recall_all"] == 1.0
    assert second["cases"][0]["metrics"]["matched_symbol_truth"] == 1
    assert second["cases"][0]["metrics"]["relevant_symbol_recall_all"] == 0.5
    assert second["cases"][0]["metrics"]["seed_file_recall_all"] == 1.0
    assert second["cases"][0]["metrics"]["seed_file_truth"] == 1

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "context"
    run("run", "--archbird", str(fake), "--label", "context", environment=environment)
    state = json.loads((root / "state.json").read_text())
    context_comparison = json.loads((root / state["current_run_comparison"]).read_text())
    assert context_comparison["cases"][0]["status"] == "unchanged"
    assert context_comparison["cases"][0]["context_status"] == "regressed"
    assert context_comparison["summary"]["reviewed_quality"]["unchanged"] == 1
    assert context_comparison["summary"]["reviewed_context"]["regressed"] == 1

    case["task"]["description"] += " Reviewed wording update."
    write_json(case_path, case)
    run("freeze", environment=environment)
    state = json.loads((root / "state.json").read_text())
    second_corpus = state["current_corpus_sha256"]
    corpus_comparison = json.loads((root / state["current_corpus_comparison"]).read_text())
    assert corpus_comparison["changed"] == ["fixture-target-change"]
    assert corpus_comparison["added"] == []
    assert corpus_comparison["removed"] == []

    (source / "src/other.py").write_text("def other():\n    return 3\n", encoding="utf-8")
    git(source, "add", "src/other.py")
    git(source, "commit", "-m", "advance reserved head")
    advanced = git(source, "rev-parse", "HEAD")
    run("reserve", environment=environment)
    state = json.loads((root / "state.json").read_text())
    second_reservation = state["current_reservation_sha256"]
    assert state["previous_reservation_sha256"] == first_reservation
    assert second_reservation != first_reservation
    reservation = json.loads(
        (root / f"reservations/{second_reservation}/reservation.json").read_text()
    )
    assert reservation["repositories"][0]["head_revision"] == advanced
    reservation_comparison = json.loads(
        (root / state["current_reservation_comparison"]).read_text()
    )
    assert reservation_comparison["changed"] == ["reserved-fixture"]
    assert reservation_comparison["added"] == []
    assert reservation_comparison["removed"] == []
    print("evaluation harness tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
