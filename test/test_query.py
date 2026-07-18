#!/usr/bin/env python3
"""Differential saved-map query tests for the native C kernel."""

from __future__ import annotations

import copy
import difflib
import importlib.util
import json
from pathlib import Path
import re
import sys

from archbird.map.analyze import analyze
from archbird.map.config import load_config
from archbird.map.query import query_map
from archbird.map.render import (
    _map_dict,
    _query_dict,
    _query_markdown,
    _render_markdown,
    render_markdown,
    render_query_markdown,
)


def load_extension(path: Path):
    spec = importlib.util.spec_from_file_location("archbird._native", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load native extension {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["archbird._native"] = module
    spec.loader.exec_module(module)
    return module


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def tool_label(tool: dict) -> str:
    return (
        f"{tool['name']} {tool['version']} "
        f"`{tool['implementation_sha256'][:16]}`"
    )


def normalized_query_report(report: str, tool: dict) -> str:
    report = report.replace(
        f"query tool {tool_label(tool)}", "query tool <native>"
    )
    return re.sub(
        r"(calls unique=[0-9]+) ambiguous=",
        r"\1 candidate=0 ambiguous=",
        report,
        count=1,
    )


def oracle_query_projection(document: dict) -> dict:
    """Remove native post-oracle route axes while retaining legacy parity."""

    projected = copy.deepcopy(document)
    for match in projected.get("test_matches", []):
        for field in (
            "confidence",
            "evidence_scope",
            "provenance",
            "seed_distance",
            "target",
            "target_role",
        ):
            match.pop(field, None)
    return projected


def native_map_report_projection(report: str) -> str:
    """Account for post-oracle labels without weakening Map IR parity."""

    report = re.sub(r"; ([0-9]+) files;", r"; \1 mapped source files;", report, count=1)
    report = re.sub(
        r"(lexical occurrences=[0-9]+ unique=[0-9]+) ambiguous=",
        r"\1 candidate=0 ambiguous=",
        report,
        count=1,
    )
    report = report.replace(
        "## Executable artifacts\n\n```text",
        "## Executable artifacts\n\nConfigured artifact records are shown; "
        "unconfigured repository outputs are outside this inventory.\n\n```text",
        1,
    )
    if "\nunique:" in report:
        return report.replace("\nambiguous:", "\ncandidate:\nambiguous:", 1)
    return report.replace(
        "ambiguous:\nunresolved:", "ambiguous:\ncandidate:\nunresolved:", 1
    )


def omit_saved_map_defaults(document: dict) -> dict:
    """Exercise schema-4/5 fields whose loader default is the empty string."""

    result = json.loads(json.dumps(document))
    if result.get("tool", {}).get("name") == "archbird":
        result["tool"].pop("name")
    for file in result.get("files", []):
        for symbol in file.get("symbols", []):
            for field in ("scope", "signature"):
                if symbol.get(field) == "":
                    symbol.pop(field)
    for package in result.get("packages", []):
        for field in ("identity", "version"):
            if package.get(field) == "":
                package.pop(field)
    for route in result.get("builds", []):
        if route.get("command") == "":
            route.pop("command")
    for component in result.get("components", []):
        if component.get("description") == "":
            component.pop("description")
    for index in result.get("indexes", []):
        if index.get("path_prefix") == "":
            index.pop("path_prefix")
        for field in ("name", "version"):
            if index.get("tool", {}).get(field) == "":
                index["tool"].pop(field)
    for diagnostic in result.get("diagnostics", []):
        if diagnostic.get("path") == "":
            diagnostic.pop("path")
    return result


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_query.py EXTENSION FIXTURE")
    extension = load_extension(Path(sys.argv[1]).resolve())
    fixture = Path(sys.argv[2]).resolve()
    data = analyze(load_config(fixture / "archbird.json", root_override=fixture))
    map_document = _map_dict(data)
    map_json = canonical(map_document)
    expected_map_report = native_map_report_projection(render_markdown(data))
    native_map_report = extension.map_markdown(map_json).decode()
    if native_map_report != expected_map_report:
        raise AssertionError(
            "native compact Map Markdown differs from reference\n"
            + "".join(
                difflib.unified_diff(
                    expected_map_report.splitlines(keepends=True),
                    native_map_report.splitlines(keepends=True),
                    fromfile="oracle",
                    tofile="native",
                )
            )
        )
    native_full_report = extension.map_markdown(map_json, full=True).decode()
    expected_full_report = native_map_report_projection(render_markdown(data, full=True))
    if native_full_report != expected_full_report:
        raise AssertionError(
            "native full Map Markdown differs from reference\n"
            + "".join(
                difflib.unified_diff(
                    expected_full_report.splitlines(keepends=True),
                    native_full_report.splitlines(keepends=True),
                    fromfile="oracle",
                    tofile="native",
                )
            )
        )
    defaulted_map_json = canonical(omit_saved_map_defaults(map_document))
    if extension.map_markdown(defaulted_map_json).decode() != expected_map_report:
        raise AssertionError("native Map Markdown rejects valid saved-map defaults")
    if extension.map_markdown(defaulted_map_json, full=True).decode() != (
        native_map_report_projection(render_markdown(data, full=True))
    ):
        raise AssertionError("native full Map Markdown rejects saved-map defaults")
    minimum_map_report = native_map_report_projection(
        _render_markdown(data, full=False, visible_paths=set())
    )
    native_bounded_report = extension.map_markdown(
        map_json, max_chars=len(minimum_map_report)
    ).decode()
    if len(native_bounded_report) > len(minimum_map_report):
        raise AssertionError("native bounded Map Markdown exceeded its budget")
    if "## Compact projection" not in native_bounded_report:
        raise AssertionError("native bounded Map Markdown omitted its ledger")
    if "canonical Map IR remains complete" not in native_bounded_report:
        raise AssertionError("native bounded Map Markdown blurred the IR/view boundary")
    try:
        extension.map_markdown(map_json, max_chars=1)
    except Exception:
        pass
    else:
        raise AssertionError("native Map Markdown accepted an undersized budget")

    oversized_document = json.loads(
        (fixture.parent / "report_map.json").read_text(encoding="utf-8")
    )
    oversized_inputs = oversized_document["artifacts"][0]["inputs"]
    oversized_inputs.extend(
        {
            "evidence": ["configured:test-budget"],
            "path": f"generated/report-input-{index:04d}.c",
        }
        for index in range(1000)
    )
    oversized_inputs.sort(key=lambda row: row["path"])
    oversized_json = canonical(oversized_document)
    for budget in (2000, 12000):
        first = extension.map_markdown(
            oversized_json, max_chars=budget
        ).decode()
        second = extension.map_markdown(
            oversized_json, max_chars=budget
        ).decode()
        if first != second:
            raise AssertionError("budgeted Map Markdown is not repeatable")
        if len(first) > budget:
            raise AssertionError(
                f"budgeted Map Markdown exceeded {budget} characters"
            )
        if "## Compact projection" not in first:
            raise AssertionError("budgeted Map Markdown has no projection ledger")
        if "Executable artifacts" not in first.split(
            "Omitted complete sections: ", 1
        )[-1]:
            raise AssertionError(
                "budgeted Map Markdown did not disclose its omitted artifact section"
            )
        if "canonical Map IR remains complete" not in first:
            raise AssertionError(
                "budgeted Map Markdown did not preserve the IR/view boundary"
            )

    wide_query_document = json.loads(
        (fixture.parent / "report_map.json").read_text(encoding="utf-8")
    )
    artifact_template = wide_query_document["artifacts"][0]
    wide_query_document["artifacts"] = []
    artifact_names = []
    for index in range(100):
        artifact = copy.deepcopy(artifact_template)
        artifact["name"] = f"artifact-{index:03d}"
        artifact["output"] = f"dist/artifact-{index:03d}.js"
        wide_query_document["artifacts"].append(artifact)
        artifact_names.append(artifact["name"])
    wide_query_json = canonical(wide_query_document)
    wide_request = canonical(
        {
            "artifacts": artifact_names,
            "depth": 1,
            "direction": "both",
            "test_depth": 1,
        }
    )
    wide_first = extension.map_query_markdown(
        wide_query_json, wide_request, max_chars=2000
    ).decode()
    wide_second = extension.map_query_markdown(
        wide_query_json, wide_request, max_chars=2000
    ).decode()
    if wide_first != wide_second or len(wide_first) > 2000:
        raise AssertionError("budgeted query projection is unstable or oversized")
    if "## Compact projection" not in wide_first:
        raise AssertionError("budgeted query projection has no omission ledger")
    if "Routed evidence" not in wide_first.split(
        "Omitted complete sections: ", 1
    )[-1]:
        raise AssertionError("budgeted query did not disclose routed evidence")
    if "…+97" not in wide_first:
        raise AssertionError("budgeted query did not compact its focus header")
    if "canonical Query IR remains complete" not in wide_first:
        raise AssertionError("budgeted query blurred the IR/view boundary")
    if data.project == "map-base":
        cases = (
            {
                "paths": ["py/pkg"],
                "direction": "both",
                "depth": 1,
                "test_depth": 8,
            },
            {
                "symbols": ["twice"],
                "direction": "upstream",
                "depth": 2,
                "test_depth": 4,
            },
            {
                "focus": ["js/index.js"],
                "direction": "downstream",
                "depth": 0,
                "test_depth": 0,
            },
        )
    else:
        cases = (
            {
                "symbols": ["poly_add"],
                "direction": "both",
                "depth": 1,
                "test_depth": 8,
            },
            {
                "packages": ["npm"],
                "direction": "both",
                "depth": 1,
                "test_depth": 8,
            },
            {
                "artifacts": ["bundle"],
                "direction": "both",
                "depth": 1,
                "test_depth": 8,
            },
            {
                "focus": ["*configured case*"],
                "direction": "both",
                "depth": 1,
                "test_depth": 8,
            },
        )
    for request in cases:
        query_data = query_map(
            data,
            focus=request.get("focus", ()),
            paths=request.get("paths", ()),
            symbols=request.get("symbols", ()),
            components=request.get("components", ()),
            packages=request.get("packages", ()),
            artifacts=request.get("artifacts", ()),
            direction=request["direction"],
            depth=request["depth"],
            test_depth=request["test_depth"],
        )
        native_document = json.loads(extension.map_query(map_json, canonical(request)))
        native = oracle_query_projection(native_document)
        expected_document = _query_dict(query_data)
        expected = dict(expected_document)
        native.pop("tool")
        expected.pop("tool")
        if native != expected:
            raise AssertionError(
                "native query differs\n"
                + json.dumps(
                    {"native": native, "expected": expected},
                    indent=2,
                    sort_keys=True,
                )
            )
        first = extension.map_query(map_json, canonical(request))
        second = extension.map_query(map_json, canonical(request))
        if first != second:
            raise AssertionError("native query bytes are not repeatable")
        native_report = extension.map_query_markdown(
            map_json, canonical(request)
        ).decode()
        expected_report = render_query_markdown(query_data)
        if normalized_query_report(
            native_report, native_document["tool"]
        ) != normalized_query_report(expected_report, expected_document["tool"]):
            raise AssertionError(
                "native query Markdown differs from reference\n"
                + "".join(
                    difflib.unified_diff(
                        normalized_query_report(
                            expected_report, expected_document["tool"]
                        ).splitlines(keepends=True),
                        normalized_query_report(
                            native_report, native_document["tool"]
                        ).splitlines(keepends=True),
                        fromfile="oracle",
                        tofile="native",
                    )
                )
            )
        minimum_query_report = _query_markdown(query_data, 0)
        bounded_report = extension.map_query_markdown(
            map_json, canonical(request), max_chars=len(native_report)
        ).decode()
        if bounded_report != native_report:
            raise AssertionError("exact-size query budget changed complete Markdown")
        if not minimum_query_report:
            raise AssertionError("oracle minimum query projection is empty")
        try:
            extension.map_query_markdown(
                map_json,
                canonical(request),
                max_chars=1,
            )
        except Exception:
            pass
        else:
            raise AssertionError("native query Markdown accepted undersized budget")
    for bad in ({}, {"paths": ["missing"]}, {"paths": ["../bad"]}):
        try:
            extension.map_query(map_json, canonical(bad))
        except Exception:
            pass
        else:
            raise AssertionError(f"invalid/unmatched query was accepted: {bad!r}")
    print(
        f"native saved-map query parity passed: {data.project}, "
        f"{len(cases)} cases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
