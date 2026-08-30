#!/usr/bin/env python3
"""Exercise external evaluation freezing, replay, and prior-run comparison."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
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
    (source / "tests/test_regression.py").write_text(
        "def test_regression():\n    assert True\n", encoding="utf-8"
    )
    git(source, "add", "src/a.py", "tests/test_regression.py")
    git(source, "commit", "-m", "change target")
    after = git(source, "rev-parse", "HEAD")

    fake_program = TEMPORARY / "fake-archbird.py"
    fake_program.write_text(
        """\
import json, os, pathlib, sys
if sys.argv[1:] == ['--version']:
  assert 'ARCHBIRD_CACHE_DIR' not in os.environ
  assert 'ARCHBIRD_CACHE_MAX_BYTES' not in os.environ
  print('0.test')
  raise SystemExit(0)
if sys.argv[1:] == ['support']:
  assert 'ARCHBIRD_CACHE_DIR' not in os.environ
  assert 'ARCHBIRD_CACHE_MAX_BYTES' not in os.environ
  print(json.dumps({
    'core_implementation_sha256':'2'*64,
    'engine':{'kind':'fixture','source':'test'},
    'frontend_implementation_sha256':'3'*64,
    'pattern':{'engine':'fixture-pattern','unicode':'fixture-unicode'},
    'providers':{'host':['fixture-host'],'portable':['fixture-portable']},
    'runtime':{
      'executable':str(pathlib.Path(sys.executable).resolve()),
      'implementation':'CPython',
      'kind':'python',
      'version':'.'.join(map(str, sys.version_info[:3])),
    },
    'version':'0.test',
  }, separators=(',', ':'), sort_keys=True))
  raise SystemExit(0)
command = sys.argv[1]
variant = os.environ.get('ARCHBIRD_EVAL_TEST_VARIANT')
out = pathlib.Path(sys.argv[sys.argv.index('--output') + 1])
out.parent.mkdir(parents=True, exist_ok=True)
if command in ('plan', 'act'):
  historical_root = pathlib.Path(
    sys.argv[sys.argv.index('--root') + 1]
  ).resolve()
  assert (historical_root / 'src/a.py').read_text() == (
    'def target():\\n    return 1\\n\\ndef helper():\\n'
    '    return target()\\n'
  )
  assert not (historical_root / 'tests/test_regression.py').exists()
  if command == 'act':
    assert pathlib.Path(
      sys.argv[sys.argv.index('--config') + 1]
    ).name == 'archbird.json'
if command == 'config':
  value = {
    'project':'fixture',
    'layers':[{'globs':['**/*.py'],'language':'python','name':'python'}],
  }
elif command == 'map':
  assert 'ARCHBIRD_CACHE_DIR' not in os.environ
  assert 'ARCHBIRD_CACHE_MAX_BYTES' not in os.environ
  cache = pathlib.Path(sys.argv[sys.argv.index('--cache-dir') + 1])
  assert sys.argv[sys.argv.index('--cache-max-bytes') + 1] == '1073741824'
  marker = cache / 'fixture-marker'
  if out.name == 'map.json':
    assert not marker.exists()
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text('warm')
  elif out.name == 'after-map.json':
    assert marker.read_text() == 'warm'
  if '--view' in sys.argv:
    assert sys.argv[sys.argv.index('--view') + 1] == 'metrics'
    assert marker.read_text() == 'warm'
    assert '--check' in sys.argv
    assert sys.argv[sys.argv.index('--detail') + 1] == 'standard'
    assert sys.argv[sys.argv.index('--max-chars') + 1] == '12000'
    assert sys.argv[sys.argv.index('--progress') + 1] == 'never'
    preset = out.stem.removeprefix('metrics-')
    assert preset in ('default', 'imports')
    if preset == 'imports':
      assert sys.argv[sys.argv.index('--relations') + 1] == 'imports'
    else:
      assert '--relations' not in sys.argv
    if variant == 'empty-metrics':
      out.write_bytes(b'')
      raise SystemExit(0)
    if variant == 'oversized-metrics':
      out.write_bytes(b'x' * 12001)
      raise SystemExit(0)
    if variant == 'mutating-metrics':
      (out.parent / 'after-map.json').write_text('{}')
    projection = ('6' if preset == 'default' else '7') * 64
    report = '''# fixture code metrics

## Declaration extent

- `file:src/a.py`: max=2 lines; measured=1; unknown=1

## Graph structure

- `file:src/a.py`: out-neighbors=>=1; in-neighbors=>=1; out-relations=>=2; in-relations=>=2; scc-size=>=1; cyclic=yes
- `file:src/other.py`: out-neighbors=>=1; in-neighbors=>=1; out-relations=>=1; in-relations=>=1; scc-size=>=2; cyclic=yes
- `file:tests/test_a.py`: out-neighbors=>=0; in-neighbors=>=1; out-relations=>=0; in-relations=>=1; scc-size=>=1; cyclic=no
- `file:tests/test_regression.py`: out-neighbors=>=0; in-neighbors=>=0; out-relations=>=0; in-relations=>=0; scc-size=>=1; cyclic=unknown (no known cycle)
- ... 3 metric rows omitted

## Group boundaries

- `layer:python`: internal-relations=>=4; inbound-relations=>=0; outbound-relations=>=1
- ... 1 group rows omitted

## Metric completeness

Result: files=4; indexed-symbols=2; entities=4; relations=5; diagnostics=0 (errors=0 warnings=0).
Evidence: metrics=incomplete; exhaustive=no; unknown=2; unsupported=1; presentation-omitted=4; projection=`''' + projection + '''`.
'''
    if variant == 'malformed-metrics':
      report = '# fixture code metrics\\n'
    out.write_text(report)
    raise SystemExit(0)
  if variant == 'fail':
    raise SystemExit(3)
  files = [
    {'path':'src/a.py','sha256':('a' if out.name == 'map.json' else 'b')*64},
    {'path':'src/other.py','sha256':'c'*64},
    {'path':'tests/test_a.py','sha256':'d'*64},
  ]
  if out.name == 'after-map.json':
    files.append({'path':'tests/test_regression.py','sha256':'e'*64})
  value = {'artifact':'map','schema_version':7,'project':'fixture','files':files,
           'source_tool':{'name':'archbird','version':'0.test',
             'implementation_sha256':'1'*64}}
elif command == 'diff':
  before_map = pathlib.Path(sys.argv[sys.argv.index('--before') + 1])
  assert before_map.name == 'map.json'
  assert pathlib.Path(sys.argv[sys.argv.index('--after') + 1]).name == 'after-map.json'
  assert '--root' not in sys.argv and '--config' not in sys.argv
  if variant == 'mutating-diff':
    before_map.write_text('{}')
  changed = 'src/a.py: ' + 'a'*64 + ' -> ' + 'b'*64
  if variant == 'bad-diff':
    changed = 'src/other.py: ' + 'c'*64 + ' -> ' + 'f'*64
  value = {'artifact':'diff','schema_version':7,'sections':{'files':{
    'added':['tests/test_regression.py'],
    'changed':[changed],
    'removed':[],
  }}}
elif command in ('query', 'impact'):
  good = variant in ('good', 'context', 'retained')
  files = ([{'distance':0,'path':'src/a.py','symbols':[{'name':'target'}]}]
           if good else [{'distance':0,'path':'src/other.py','symbols':[{'name':'other'}]}])
  if variant == 'context':
    files[0]['symbols'].append({'name':'direct_caller'})
  matches = ([{'path':'tests/test_a.py','selector':'test_target'}] if good else [])
  if variant == 'retained':
    files.append({
      'distance':2,
      'path':'src/canonical-candidate.py',
      'symbols':[{'name':'candidate_symbol'}],
    })
    matches.append({
      'confidence':'conservative',
      'evidence_scope':'file',
      'path':'tests/test_canonical_candidate.py',
      'provenance':'derived',
      'seed_distance':2,
      'selector':'test_candidate',
      'target':'src/canonical-candidate.py',
      'target_role':'transitive-context',
    })
  matched = ([{'path':'src/a.py','name':'target'}] if good else [])
  value = {'artifact':'query','schema_version':7,'files':files,
           'query':{'context':{}},
           'matched_symbols':matched,'test_matches':matches}
elif command == 'path':
  value = {
    'artifact':'path',
    'outcome':'found',
    'reason':'witnesses',
    'paths':[{
      'nodes':['file:src/a.py','file:src/other.py'],
      'state':'current',
    }],
    'search':{'shortest_length':1},
  }
elif command == 'verify':
  before = pathlib.Path.cwd().name == 'before'
  variant = os.environ.get('ARCHBIRD_EVAL_TEST_VARIANT')
  findings = ([{
    'comparison':'missing',
    'evidence_state':'current',
    'fingerprint':'4'*64,
    'key':'tests/test_regression.py::test_regression',
  }] if before else [])
  if before and variant == 'bad':
    findings.append({
      'comparison':'missing',
      'evidence_state':'current',
      'fingerprint':'5'*64,
      'key':'tests/test_unrelated.py::test_unrelated',
    })
  value = {'artifact':'verification','schema_version':2,'constraints':[
    {
    'findings':findings,
    'id':'HISTORICAL-INTRODUCED-TESTS',
    'status':'fail' if before else 'pass',
  }]}
elif command == 'plan':
  value = {'artifact':'plan','schema_version':9,'items':[{
    'executable':True,
    'id':'test-regression',
    'origins':[{'issue_fingerprint':'4'*64}],
    'operation':{
      'action':'create_file',
      'candidate_paths':['tests/test_regression.py'],
      'path':'tests/test_regression.py',
    },
    'acceptance':{
      'constraints':['HISTORICAL-INTRODUCED-TESTS'],
      'projection_deltas':[],
    },
  }]}
elif command == 'act':
  value = {
    'artifact':'act',
    'schema_version':5,
    'state':'accepted',
  }
else:
  raise SystemExit(f'unsupported fake command: {command}')
out.write_text(json.dumps(value, separators=(',', ':'), sort_keys=True) + '\\n')
""",
        encoding="utf-8",
    )
    fake = TEMPORARY / "fake-archbird"
    fake.write_text(
        "#!/bin/sh\nexec "
        f"{shlex.quote(str(Path(sys.executable).resolve()))} "
        f"{shlex.quote(str(fake_program.resolve()))} \"$@\"\n",
        encoding="utf-8",
    )
    fake.chmod(0o755)

    environment = os.environ.copy()
    environment["ARCHBIRD_EVAL_ROOT"] = str(root)
    missing = os.environ.copy()
    missing.pop("ARCHBIRD_EVAL_ROOT", None)
    assert run("init", environment=missing, check=False).returncode == 2
    run("init", environment=environment)
    legacy_state = json.loads((root / "state.json").read_text())
    for key in (
        "held_out_opened_corpus_sha256",
        "held_out_opened_label",
        "held_out_run_sha256",
    ):
        legacy_state.pop(key)
    write_json(root / "state.json", legacy_state)
    run("show", environment=environment)
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
            "issue_query_source": "task_title",
            "issue_search_limit": 20,
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
            "line_budgets": [20, 100],
            "performance": "observational_single_sample",
            "retrieval": ["mrr", "recall_all", "recall_at_20"],
            "split_aggregation": "separate",
        },
        "operations": {
            "diff": {"inputs": "saved_maps"},
            "metrics": {
                "cache_state": "warm_after_map",
                "max_chars": 12000,
                "presets": ["default", "imports"],
            },
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
            "introduced_tests": ["tests/test_regression.py::test_regression"],
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
    held_out_case = json.loads(json.dumps(case))
    held_out_case["id"] = "fixture-held-out-change"
    held_out_case["split"] = "held_out"
    write_json(root / "authoring/cases/fixture-held-out-change.json", held_out_case)
    path_case = json.loads(json.dumps(case))
    path_case["id"] = "zz-fixture-path-change"
    path_case["analysis"] = {
        "direction": "downstream",
        "level": "file",
        "max_depth": 4,
        "max_paths": 4,
        "relations": ["references"],
        "seed_rationale": "The task names both historical endpoints.",
        "source": {"kind": "file", "value": "src/a.py"},
        "target": {"kind": "file", "value": "src/other.py"},
        "track": "exact_path",
    }
    path_case["ground_truth"]["path"] = {
        "nodes": [["file:src/a.py", "file:src/other.py"]],
        "outcome": "found",
        "path_state": "current",
        "reason": "witnesses",
        "shortest_length": 1,
    }
    path_case["review"] = {
        "rationale": "Synthetic exact-Path evaluator integration fixture.",
        "status": "candidate",
    }
    path_case["task"] = {
        "description": "Explain the exact historical endpoint connection.",
        "source": "fixture:path",
        "title": "Connect a.py to other.py",
    }
    write_json(root / "authoring/cases/zz-fixture-path-change.json", path_case)
    run("sync", "--fetch", "--reservations", "validation", environment=environment)
    run("validate", environment=environment)
    run("freeze", environment=environment)
    state = json.loads((root / "state.json").read_text())
    first_corpus = state["current_corpus_sha256"]
    corpus = json.loads((root / f"corpora/{first_corpus}/corpus.json").read_text())
    assert corpus["cases"][0]["diff_files"] == [
        "src/a.py",
        "tests/test_regression.py",
    ]

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "bad"
    environment["ARCHBIRD_CACHE_DIR"] = str(TEMPORARY / "ambient-cache")
    environment["ARCHBIRD_CACHE_MAX_BYTES"] = "7"
    run("run", "--archbird", str(fake), "--label", "bad", environment=environment)
    first_run = json.loads((root / "state.json").read_text())["current_run_sha256"]
    first = json.loads((root / f"runs/{first_run}/result.json").read_text())
    run_schema = json.loads(
        (REPOSITORY / "schema/evaluation-run.schema.json").read_text()
    )
    assert set(first).issubset(run_schema["properties"])
    assert first["schema_version"] == 5
    assert first["operations"] == {
        "diff": {"inputs": "saved_maps"},
        "metrics": {
            "cache_state": "warm_after_map",
            "max_chars": 12000,
            "presets": ["default", "imports"],
        },
    }
    assert first["cache_policy"] == {
        "provider_cache": {
            "initial_state": "empty",
            "max_bytes": 1_073_741_824,
            "mode": "isolated_per_case",
            "reuse": "before_and_after_revision",
        }
    }
    assert first["held_out_opened"] is False
    assert first["cases"][0]["metrics"]["relevant_file_recall_all"] == 0.0
    diff_result = first["cases"][0]
    assert diff_result["operations"]["diff"]["status"] == "ok"
    assert diff_result["operations"]["diff"]["file_section_exact"] is True
    assert diff_result["operations"]["diff"]["map_git_consistency"] == {
        "git_changed_unmapped_paths": [],
        "git_mapped_paths_without_content_change": [],
        "map_content_paths_not_in_git": [],
        "mapped_content_paths_exact": True,
    }
    assert diff_result["operations"]["diff"]["section_counts"]["files"] == {
        "added": 1,
        "changed": 1,
        "removed": 0,
    }
    assert diff_result["artifacts"]["map_diff"]["bytes"] > 0
    assert diff_result["metrics"]["map_diff_file_section_exact"] == 1.0
    assert diff_result["metrics"]["map_diff_map_git_content_paths_exact"] == 1.0
    assert "impact_git_file_recall_all" in diff_result["metrics"]
    assert not any(name.startswith("diff_file") for name in diff_result["metrics"])
    metrics_operation = diff_result["operations"]["metrics"]
    assert metrics_operation["status"] == "ok"
    assert metrics_operation["cache_state"] == "warm_after_map"
    assert metrics_operation["max_chars"] == 12000
    assert set(metrics_operation["presets"]) == {"default", "imports"}
    default_metrics = metrics_operation["presets"]["default"]["report"]
    assert default_metrics["bytes"] <= 12000
    assert default_metrics["completeness"] == {
        "exhaustive": False,
        "metrics": "incomplete",
        "unknown": 2,
        "unsupported": 1,
    }
    assert default_metrics["rows"] == {
        "declaration": {"displayed": 1, "omitted": 0},
        "graph": {"displayed": 4, "omitted": 3},
        "group": {"displayed": 1, "omitted": 1},
    }
    assert default_metrics["cycles"] == {
        "incomplete": 4,
        "multi_node": 1,
        "noncyclic": 1,
        "singleton_cyclic": 1,
        "unknown": 1,
    }
    assert (
        diff_result["artifacts"]["metrics_default"]["sha256"]
        == (default_metrics["sha256"])
    )
    assert diff_result["artifacts"]["metrics_imports"]["bytes"] <= 12000
    assert diff_result["metrics"]["metrics_default_output_bytes"] > 0
    assert diff_result["metrics"]["metrics_imports_output_bytes"] > 0
    path_result = next(
        row for row in first["cases"] if row["id"] == "zz-fixture-path-change"
    )
    assert path_result["metrics"]["path_accuracy"] == 1.0
    assert path_result["metrics"]["path_node_recall_all"] == 1.0
    assert path_result["artifacts"]["path"]["bytes"] > 0
    assert first["tool"]["launcher"] == {
        "path": str(fake.resolve()),
        "sha256": __import__("hashlib").sha256(fake.read_bytes()).hexdigest(),
    }
    support = first["tool"]["support"]
    assert support["status"] == "available"
    assert support["report"]["runtime"]["kind"] == "python"
    assert support["report"]["runtime"]["executable"] == str(Path(sys.executable).resolve())
    assert first["tool"]["support"]["report"]["runtime"]["version"] == (
        ".".join(map(str, sys.version_info[:3]))
    )

    schema_three_run = json.loads(json.dumps(first))
    schema_three_run["schema_version"] = 3
    schema_three_run.pop("operations")
    for result in schema_three_run["cases"]:
        result.pop("operations", None)
        result.get("artifacts", {}).pop("map_diff", None)
        metrics = result.get("metrics", {})
        for current, legacy in (
            ("impact_git_context_file_", "diff_context_file_"),
            ("impact_git_file_context_", "diff_file_context_"),
            ("impact_git_file_", "diff_file_"),
        ):
            for name in list(metrics):
                if name.startswith(current):
                    metrics[legacy + name[len(current) :]] = metrics.pop(name)
    schema_three_bytes = (
        json.dumps(
            schema_three_run,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()
    schema_three_sha256 = __import__("hashlib").sha256(
        schema_three_bytes
    ).hexdigest()
    schema_three_path = root / f"runs/{schema_three_sha256}/result.json"
    schema_three_path.parent.mkdir()
    schema_three_path.write_bytes(schema_three_bytes)
    schema_three_comparison_path = root / "schema-three-comparison.json"
    run(
        "compare",
        "--before",
        schema_three_sha256,
        "--after",
        first_run,
        "--output",
        str(schema_three_comparison_path),
        environment=environment,
    )
    schema_three_comparison = json.loads(
        schema_three_comparison_path.read_text()
    )
    schema_three_metrics = schema_three_comparison["cases"][0]["metrics"]
    assert schema_three_metrics["impact_git_file_recall_all"][
        "classification"
    ] == "unchanged"
    assert not any(name.startswith("diff_file") for name in schema_three_metrics)

    incompatible_run = json.loads(json.dumps(first))
    incompatible_run["cache_policy"]["provider_cache"]["max_bytes"] //= 2
    incompatible_bytes = (
        json.dumps(
            incompatible_run,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()
    incompatible_sha256 = __import__("hashlib").sha256(
        incompatible_bytes
    ).hexdigest()
    incompatible_path = root / f"runs/{incompatible_sha256}/result.json"
    incompatible_path.parent.mkdir()
    incompatible_path.write_bytes(incompatible_bytes)
    incompatible_comparison_path = root / "incompatible-comparison.json"
    run(
        "compare",
        "--before",
        first_run,
        "--after",
        incompatible_sha256,
        "--output",
        str(incompatible_comparison_path),
        environment=environment,
    )
    incompatible_comparison = json.loads(
        incompatible_comparison_path.read_text()
    )
    assert incompatible_comparison["cache_relation"] == "changed"
    assert incompatible_comparison["performance_environment_relation"] == "same"
    assert incompatible_comparison["cases"][0]["performance_status"] == (
        "incomparable"
    )
    assert incompatible_comparison["cases"][0]["metrics"][
        "map_duration_ms"
    ]["classification"] == "incomparable"

    runtime_incompatible_run = json.loads(json.dumps(first))
    runtime_incompatible_run["tool"]["support"]["report"]["runtime"][
        "version"
    ] = "different-runtime"
    runtime_incompatible_bytes = (
        json.dumps(
            runtime_incompatible_run,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()
    runtime_incompatible_sha256 = __import__("hashlib").sha256(
        runtime_incompatible_bytes
    ).hexdigest()
    runtime_incompatible_path = (
        root / f"runs/{runtime_incompatible_sha256}/result.json"
    )
    runtime_incompatible_path.parent.mkdir()
    runtime_incompatible_path.write_bytes(runtime_incompatible_bytes)
    runtime_comparison_path = root / "runtime-incompatible-comparison.json"
    run(
        "compare",
        "--before",
        first_run,
        "--after",
        runtime_incompatible_sha256,
        "--output",
        str(runtime_comparison_path),
        environment=environment,
    )
    runtime_comparison = json.loads(runtime_comparison_path.read_text())
    assert runtime_comparison["cache_relation"] == "same"
    assert runtime_comparison["performance_environment_relation"] == "changed"
    assert runtime_comparison["cases"][0]["performance_status"] == "incomparable"
    assert runtime_comparison["cases"][0]["metrics"][
        "map_duration_ms"
    ]["classification"] == "incomparable"
    assert runtime_comparison["cases"][0]["metrics"][
        "relevant_file_recall_all"
    ]["classification"] == "unchanged"

    evaluator_incompatible_run = json.loads(json.dumps(first))
    evaluator_incompatible_run["evaluator"]["implementation_sha256"] = "f" * 64
    evaluator_incompatible_bytes = (
        json.dumps(
            evaluator_incompatible_run,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()
    evaluator_incompatible_sha256 = __import__("hashlib").sha256(
        evaluator_incompatible_bytes
    ).hexdigest()
    evaluator_incompatible_path = (
        root / f"runs/{evaluator_incompatible_sha256}/result.json"
    )
    evaluator_incompatible_path.parent.mkdir()
    evaluator_incompatible_path.write_bytes(evaluator_incompatible_bytes)
    evaluator_comparison_path = root / "evaluator-incompatible-comparison.json"
    run(
        "compare",
        "--before",
        first_run,
        "--after",
        evaluator_incompatible_sha256,
        "--output",
        str(evaluator_comparison_path),
        environment=environment,
    )
    evaluator_comparison = json.loads(evaluator_comparison_path.read_text())
    assert evaluator_comparison["cache_relation"] == "same"
    assert evaluator_comparison["performance_environment_relation"] == "changed"
    assert evaluator_comparison["cases"][0]["performance_status"] == "incomparable"
    assert evaluator_comparison["cases"][0]["metrics"][
        "map_duration_ms"
    ]["classification"] == "incomparable"
    assert evaluator_comparison["cases"][0]["metrics"][
        "relevant_file_recall_all"
    ]["classification"] == "unchanged"

    legacy_run = json.loads(json.dumps(first))
    legacy_run["schema_version"] = 2
    legacy_run["host"] = {
        "machine": first["host"]["machine"],
        "platform": first["host"]["platform"],
        "python": first["host"]["runtime"]["version"],
    }
    legacy_bytes = (
        json.dumps(
            legacy_run,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()
    legacy_sha256 = __import__("hashlib").sha256(legacy_bytes).hexdigest()
    legacy_path = root / f"runs/{legacy_sha256}/result.json"
    legacy_path.parent.mkdir()
    legacy_path.write_bytes(legacy_bytes)
    legacy_comparison_path = root / "legacy-comparison.json"
    run(
        "compare",
        "--before",
        legacy_sha256,
        "--after",
        first_run,
        "--output",
        str(legacy_comparison_path),
        environment=environment,
    )
    legacy_comparison = json.loads(legacy_comparison_path.read_text())
    assert legacy_comparison["performance_environment_relation"] == "unknown"
    assert legacy_comparison["cases"][0]["performance_status"] == "incomparable"
    assert legacy_comparison["cases"][0]["metrics"][
        "relevant_file_recall_all"
    ]["classification"] == "unchanged"

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "good"
    run("run", "--archbird", str(fake), "--label", "good", environment=environment)
    state = json.loads((root / "state.json").read_text())
    second_run = state["current_run_sha256"]
    assert state["previous_run_sha256"] == first_run
    comparison_path = root / state["current_run_comparison"]
    comparison = json.loads(comparison_path.read_text())
    assert comparison["schema_version"] == 3
    assert comparison["cache_relation"] == "same"
    assert comparison["performance_environment_relation"] == "same"
    row = comparison["cases"][0]
    assert row["metrics"]["relevant_file_recall_all"]["classification"] == "improved"
    assert row["metrics"]["relevant_test_recall_all"]["classification"] == "improved"
    assert row["metrics"]["verification_false_findings"]["classification"] == "improved"
    assert (
        row["metrics"]["verification_transition_accuracy"]["classification"]
        == "improved"
    )
    assert comparison["summary"]["reviewed_quality"]["improved"] == 1
    second = json.loads((root / f"runs/{second_run}/result.json").read_text())
    assert (
        second["cases"][0]["artifacts"]["map_diff"]["sha256"]
        == first["cases"][0]["artifacts"]["map_diff"]["sha256"]
    )
    for name in ("metrics_default", "metrics_imports"):
        assert (
            second["cases"][0]["artifacts"][name]["sha256"]
            == first["cases"][0]["artifacts"][name]["sha256"]
        )
    assert second["aggregate"]["reviewed_by_split"]["development"]["cases"] == 1
    assert second["aggregate"]["reviewed_by_split"]["held_out"]["cases"] == 0
    assert second["aggregate"]["reviewed_by_split"]["development"]["metrics_pooled"][
        "relevant_file"
    ] == {
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
    assert second["cases"][0]["metrics"]["issue_file_recall_all"] == 0.5
    assert second["cases"][0]["metrics"]["issue_context_file_recall_all"] == 1.0
    assert second["cases"][0]["metrics"]["issue_test_recall_all"] == 1.0
    assert second["cases"][0]["artifacts"]["issue_query"]["bytes"] > 0
    assert second["cases"][0]["metrics"]["seed_file_recall_all"] == 1.0
    assert second["cases"][0]["metrics"]["seed_file_truth"] == 1
    assert second["cases"][0]["metrics"]["verification_before_missing_recall"] == 1.0
    assert second["cases"][0]["metrics"]["verification_after_presence_recall"] == 1.0
    assert second["cases"][0]["metrics"]["verification_false_findings"] == 0
    assert second["cases"][0]["metrics"]["verification_transition_accuracy"] == 1.0
    assert second["cases"][0]["metrics"]["plan_generation_success"] == 1.0
    assert second["cases"][0]["metrics"]["plan_items"] == 1
    assert second["cases"][0]["metrics"]["plan_executable_items"] == 1
    assert second["cases"][0]["metrics"]["plan_non_executable_items"] == 0
    assert second["cases"][0]["metrics"]["plan_manual_items"] == 0
    assert second["cases"][0]["metrics"]["plan_executable_fraction"] == 1.0
    assert second["cases"][0]["metrics"]["plan_origin_recall"] == 1.0
    assert second["cases"][0]["metrics"]["plan_after_acceptance_satisfied"] == 1.0
    assert second["cases"][0]["metrics"]["act_preview_available"] == 1.0

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "retained"
    run("run", "--archbird", str(fake), "--label", "retained", environment=environment)
    state = json.loads((root / "state.json").read_text())
    retained_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    retained_comparison = json.loads(
        (root / state["current_run_comparison"]).read_text()
    )
    assert retained_comparison["cases"][0]["status"] == "unchanged"
    assert retained_comparison["cases"][0]["context_status"] == "unchanged"
    assert retained_comparison["summary"]["reviewed_context"]["unchanged"] == 1
    assert retained_run["cases"][0]["metrics"][
        "canonical_file_evidence_returned"
    ] == 2
    assert retained_run["cases"][0]["metrics"][
        "canonical_test_evidence_returned"
    ] == 2
    assert retained_run["cases"][0]["metrics"]["relevant_file_returned"] == 1
    assert retained_run["cases"][0]["metrics"]["relevant_test_returned"] == 1

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

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "bad-diff"
    rejected_diff = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "bad-public-diff",
        environment=environment,
        check=False,
    )
    assert rejected_diff.returncode == 2
    state = json.loads((root / "state.json").read_text())
    rejected_diff_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    assert all(
        result["error"] == "public Diff file section differs from its input Maps"
        and result["operations"]["diff"]["file_section_exact"] is False
        for result in rejected_diff_run["cases"]
    )
    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "mutating-diff"
    mutated_diff = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "mutating-public-diff",
        environment=environment,
        check=False,
    )
    assert mutated_diff.returncode == 2
    state = json.loads((root / "state.json").read_text())
    mutated_diff_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    assert all(
        result["error"] == "public Diff mutated a saved Map input"
        for result in mutated_diff_run["cases"]
    )

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "empty-metrics"
    empty_metrics = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "empty-public-metrics",
        environment=environment,
        check=False,
    )
    assert empty_metrics.returncode == 2
    state = json.loads((root / "state.json").read_text())
    empty_metrics_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    assert all(
        result["error"] == "public Metrics report is empty"
        for result in empty_metrics_run["cases"]
    )

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "oversized-metrics"
    oversized_metrics = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "oversized-public-metrics",
        environment=environment,
        check=False,
    )
    assert oversized_metrics.returncode == 2
    state = json.loads((root / "state.json").read_text())
    oversized_metrics_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    assert all(
        result["error"] == "public Metrics report exceeds max_chars (12001 > 12000)"
        for result in oversized_metrics_run["cases"]
    )

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "malformed-metrics"
    malformed_metrics = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "malformed-public-metrics",
        environment=environment,
        check=False,
    )
    assert malformed_metrics.returncode == 2
    state = json.loads((root / "state.json").read_text())
    malformed_metrics_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    assert all(
        result["error"] == "public Metrics report has no stable result tail"
        for result in malformed_metrics_run["cases"]
    )

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "mutating-metrics"
    mutated_metrics = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "mutating-public-metrics",
        environment=environment,
        check=False,
    )
    assert mutated_metrics.returncode == 2
    state = json.loads((root / "state.json").read_text())
    mutated_metrics_run = json.loads(
        (root / f"runs/{state['current_run_sha256']}/result.json").read_text()
    )
    assert all(
        result["error"] == "public Metrics mutated a primary artifact"
        for result in mutated_metrics_run["cases"]
    )

    failed_root = TEMPORARY / "failed-held-out-open"
    shutil.copytree(root, failed_root)
    failed_environment = {**environment, "ARCHBIRD_EVAL_ROOT": str(failed_root)}
    failed_environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "fail"
    failed = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "failed-held-out-score",
        "--open-held-out",
        environment=failed_environment,
        check=False,
    )
    assert failed.returncode == 2
    failed_claim = json.loads((failed_root / "held-out-opened.json").read_text())
    assert failed_claim["label"] == "failed-held-out-score"
    failed_retry = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "forbidden-failed-retry",
        environment=failed_environment,
        check=False,
    )
    assert failed_retry.returncode == 2
    assert "already opened" in failed_retry.stderr

    environment["ARCHBIRD_EVAL_TEST_VARIANT"] = "context"
    run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "held-out-score",
        "--open-held-out",
        environment=environment,
    )
    state = json.loads((root / "state.json").read_text())
    held_out_run = json.loads(
        (root / f"runs/{state['held_out_run_sha256']}/result.json").read_text()
    )
    assert held_out_run["held_out_opened"] is True
    assert held_out_run["aggregate"]["reviewed_by_split"]["development"]["cases"] == 1
    assert held_out_run["aggregate"]["reviewed_by_split"]["held_out"]["cases"] == 1
    claim = json.loads((root / "held-out-opened.json").read_text())
    assert claim["corpus_sha256"] == state["held_out_opened_corpus_sha256"]
    assert claim["label"] == "held-out-score"
    blocked = run(
        "run",
        "--archbird",
        str(fake),
        "--label",
        "forbidden-retry",
        environment=environment,
        check=False,
    )
    assert blocked.returncode == 2
    assert "already opened" in blocked.stderr
    assert run("freeze", environment=environment, check=False).returncode == 2
    assert run("reserve", environment=environment, check=False).returncode == 2
    print("evaluation harness tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
