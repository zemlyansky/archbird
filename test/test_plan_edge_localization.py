#!/usr/bin/env python3
"""Pin exact dependency witnesses emitted by the native Plan compiler."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest

from archbird.native import Project, compile_plan_json


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "test" / "fixtures" / "plan_act" / "dependency_redirect"


class PlanEdgeLocalizationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.root = Path(self.temporary.name)
        shutil.copytree(FIXTURE, self.root, dirs_exist_ok=True)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def compile_plan(self) -> dict[str, object]:
        project = Project.from_repository(
            self.root,
            config=self.root / "archbird.json",
            cache_dir=None,
            map_cache=False,
        )
        map_json = project.map_json()
        verification_json = project.verify_json()
        return json.loads(
            compile_plan_json(project, map_json, verification_json)
        )

    def test_forbidden_edge_retains_exact_inducing_site(self) -> None:
        source = self.root / "src" / "ui" / "view.c"
        source_bytes = source.read_bytes()
        plan = self.compile_plan()

        self.assertEqual(len(plan["items"]), 1)
        item = plan["items"][0]
        self.assertFalse(item["executable"])
        self.assertEqual(
            item["statement"],
            "Redirect dependency ui -[import]-> storage.",
        )
        self.assertEqual(
            item["operation"],
            {
                "action": "manual",
                "candidate_paths": ["src/ui/view.c"],
                "candidate_sites": [
                    {
                        "before": "storage/raw.h",
                        "end_byte": 23,
                        "fact_id": item["operation"]["candidate_sites"][0][
                            "fact_id"
                        ],
                        "line": 1,
                        "name": "storage/raw.h",
                        "path": "src/ui/view.c",
                        "source_sha256": hashlib.sha256(
                            source_bytes
                        ).hexdigest(),
                        "start_byte": 10,
                    }
                ],
                "instructions": (
                    "Select a reviewed replacement dependency route and "
                    "rewrite every exact inducing source site."
                ),
            },
        )

    def test_repeated_imports_remain_distinct_exact_sites(self) -> None:
        source = self.root / "src" / "ui" / "view.c"
        source.write_text(
            '#include "storage/raw.h"\n'
            '#include "storage/raw.h"\n\n'
            "int render_value(void) { return raw_value(); }\n"
        )
        source_bytes = source.read_bytes()
        plan = self.compile_plan()

        sites = plan["items"][0]["operation"]["candidate_sites"]
        self.assertEqual(len(sites), 2)
        self.assertEqual([site["line"] for site in sites], [1, 2])
        self.assertEqual([site["start_byte"] for site in sites], [10, 35])
        self.assertEqual([site["end_byte"] for site in sites], [23, 48])
        self.assertEqual(
            {site["source_sha256"] for site in sites},
            {hashlib.sha256(source_bytes).hexdigest()},
        )
        self.assertEqual(
            {site["before"] for site in sites},
            {"storage/raw.h"},
        )
        self.assertEqual(len({site["fact_id"] for site in sites}), 2)

    def test_python_import_retains_statement_span_and_semantic_name(self) -> None:
        root = self.root / "python-project"
        (root / "app").mkdir(parents=True)
        (root / "ui").mkdir()
        source = root / "app/main.py"
        source.write_text(
            "from ui.widget import render\n\n"
            "def run():\n"
            "    return render()\n"
        )
        (root / "ui/widget.py").write_text(
            "def render():\n"
            '    return "ok"\n'
        )
        (root / "archbird.json").write_text(
            json.dumps(
                {
                    "project": "python-edge-plan",
                    "layers": [
                        {
                            "name": "python",
                            "language": "python",
                            "globs": ["**/*.py"],
                            "import_roots": ["."],
                        }
                    ],
                    "components": [
                        {"name": "app", "paths": ["app/**"]},
                        {"name": "ui", "paths": ["ui/**"]},
                    ],
                    "constraints": {
                        "NO-CROSS-COMPONENT-IMPORTS": {
                            "kind": "allowed_component_edges",
                            "edges": [],
                            "kinds": ["import"],
                            "owner": "architecture",
                            "rationale": "Components remain independent.",
                        }
                    },
                }
            )
        )
        project = Project.from_repository(
            root,
            config=root / "archbird.json",
            cache_dir=None,
            map_cache=False,
        )
        plan = json.loads(
            compile_plan_json(
                project, project.map_json(), project.verify_json()
            )
        )

        site = plan["items"][0]["operation"]["candidate_sites"][0]
        self.assertEqual(site["path"], "app/main.py")
        self.assertEqual(site["name"], "ui.widget")
        self.assertEqual(site["before"], "from ui.widget import render")
        self.assertEqual(site["start_byte"], 0)
        self.assertEqual(site["end_byte"], 28)
        self.assertEqual(
            site["source_sha256"],
            hashlib.sha256(source.read_bytes()).hexdigest(),
        )


if __name__ == "__main__":
    unittest.main()
