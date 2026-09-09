#!/usr/bin/env python3
"""Tree-sitter recovery must weaken only overlapping normalized facts."""

from __future__ import annotations

import hashlib
import json

from archbird import _native
from archbird.native import query_map_json


SOURCE = b"""\
int stable() { return 1; }
int broken() { auto x = ; helper(); }
"""


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def c_syntax_regressions() -> None:
    from test_compile_commands import build_map

    source = b'''static _Thread_local int value = 0;
const char *text = "_Thread_local";
/* _Thread_local must not alter source text or offsets. */
int get(void) { return value; }
'''
    mapped = build_map(_native, {"sample.c": source}, {
        "project": "compile-commands",
        "layers": [{"name": "core", "language": "c", "globs": ["*.c"]}],
    })
    errors = [d for d in mapped["diagnostics"]
              if d["code"] in ("tree-sitter-error", "tree-sitter-missing")]
    assert not errors, errors
    symbols = mapped["files"][0]["symbols"]
    assert any(s["name"] == "get" and s["line"] == 4 for s in symbols), symbols


def c_macro_regressions() -> None:
    from test_compile_commands import build_map

    config = {"project": "compile-commands", "layers": [
        {"name": "core", "language": "c", "globs": ["*.c", "*.h"]}]}
    cases = {
        "macros.c": b'''#define CASE(s, n) static void s##_##n(void)
#define check_case(s, n) static void s##_##n(void)
CASE(group, 2d_example) { typedef int Local; helper(); }
CASE(group, other) { typedef int Local; helper(); }
check_case(group, third) { helper(); }
int unaffected(void) { return 1; }
''',
        "embedded.c": b'''EM_JS(int, browser_value, (), {
  const values = [1, 2];
  return values.map(x => x + 1)[0];
})
OTHER_LANGUAGE(void, custom, (), { foreign => nonsense; })
int unaffected(void) { return 1; }
''',
        "api.h": b'''#ifdef __cplusplus
extern "C" {
#endif
int api(void);
#ifdef __cplusplus
}
#endif
''',
    }
    mapped = build_map(_native, cases, config)
    errors = [d for d in mapped["diagnostics"]
              if d["code"] in ("tree-sitter-error", "tree-sitter-missing")]
    assert not errors, errors
    files = {f["path"]: f for f in mapped["files"]}
    macro_symbols = files["macros.c"]["symbols"]
    assert [s["name"] for s in macro_symbols if s["kind"] == "function"] == ["unaffected"]
    local_types = [s for s in macro_symbols if s["kind"] == "typedef"]
    assert len(local_types) == 2 and len({s["name"] for s in local_types}) == 2, local_types
    assert all(s["name"].startswith("block@") and s["scope"] == "local" for s in local_types), local_types
    assert [s["name"] for s in files["embedded.c"]["symbols"]] == ["unaffected"]
    assert [s["name"] for s in files["api.h"]["symbols"]] == ["api"]
    assert {d["path"] for d in mapped["diagnostics"]
            if d["code"] == "c-macro-expansion-unknown"} == {"macros.c", "embedded.c"}
    broken = build_map(_native, {"broken.c": b"int broken(void) { return +; }\n"}, config)
    assert any(d["code"] == "tree-sitter-error" for d in broken["diagnostics"]), broken


def main() -> int:
    c_syntax_regressions()
    c_macro_regressions()
    path = "src/recovered.cpp"
    manifest = {
        "artifact": "archbird-source-manifest",
        "files": [
            {
                "bytes": len(SOURCE),
                "language": "cpp",
                "layer": "core",
                "path": path,
                "roles": ["source"],
                "sha256": hashlib.sha256(SOURCE).hexdigest(),
            }
        ],
        "producer": {
            "implementation_sha256": "1" * 64,
            "name": "syntax-recovery-fixture",
            "version": "1",
        },
        "project": "syntax-recovery",
        "schema_version": 1,
    }
    project = _native.project_create(canonical(manifest))
    _native.project_add_source(project, path, SOURCE)
    _native.project_finalize_sources(project)
    _native.project_set_config(
        project,
        canonical(
            {
                "layers": [
                    {
                        "globs": ["src/**/*.cpp"],
                        "language": "cpp",
                        "name": "core",
                    }
                ],
                "project": "syntax-recovery",
            }
        ),
    )
    _native.project_scan_builtin_provider(
        project, "syntax:tree-sitter:cpp", "primary"
    )
    _native.project_finalize_providers(project)
    mapped_bytes = _native.project_map(project)
    mapped = json.loads(mapped_bytes)
    symbols = {
        row["name"]: row
        for file in mapped["files"]
        for row in file["symbols"]
    }
    if symbols["broken"].get("syntax_recovery") != "intersects":
        raise AssertionError(symbols["broken"])
    if "syntax_recovery" in symbols["stable"]:
        raise AssertionError(symbols["stable"])

    focused = json.loads(
        query_map_json(
            mapped_bytes,
            symbols=[f"{path}:broken"],
            depth=0,
            test_depth=0,
        )
    )
    if focused["matched_symbols"] != [
        {
            "kind": "function",
            "line": 2,
            "name": "broken",
            "path": path,
            "scope": "",
            "signature": "",
            "syntax_recovery": "intersects",
        }
    ]:
        raise AssertionError(focused["matched_symbols"])
    print("per-fact syntax recovery passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
