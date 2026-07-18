#!/usr/bin/env python3
"""Compare native JS/TS/Vue facts with the immutable schema-6 scanner."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import subprocess
import sys

from archbird.map.scanners import js_file_facts, vue_script_text


CASES = (
    (
        "objects",
        "javascript",
        "const io = { read: { file(path) { return wasm._core_read(path) } } }\n"
        "function run(worker) { worker.postMessage({ type: 'run' }) }\n"
        "module.exports = { io, run }\n",
    ),
    (
        "typed",
        "typescript",
        "export interface Shape { area(): number }\n"
        "export type Handler = (x: number) => void\n"
        "export enum Color { Red, Blue }\n"
        "export class Circle {\n"
        "  area(): number { return 1 }\n"
        "  describe(): {x: number} { return {x: 1} }\n"
        "  scale = (factor: number): Circle => new Circle()\n"
        "  plain = () => 1\n"
        "}\nclass Widget {}\nexport default Widget\n",
    ),
    (
        "regex",
        "javascript",
        r"input.replace(/https?:\/\//g, ''); wasm.ccall('after_regex'); "
        "postMessage({type: 'done'}); const quotient = left / right;\n",
    ),
    (
        "modules",
        "javascript",
        "const dep = require('./dep');\n"
        "import side from 'side'; import('dynamic');\n"
        "export const local = () => dep.run();\n"
        "export { local as alias }; exports.extra = local;\n",
    ),
    (
        "messages",
        "javascript",
        "if (data._cmd === 'cancel') stop();\n"
        "switch (event.type) { case 'run': go(); break; case 'stop': stop(); }\n",
    ),
    (
        "vue",
        "vue",
        "<template><div/></template>\n<script setup>\n"
        "function run() { postMessage({type:'vue'}) }\n</script>\n",
    ),
)


def project(document: dict) -> tuple:
    symbols = []
    calls: Counter[str] = Counter()
    methods: Counter[str] = Counter()
    imports: set[str] = set()
    exports: set[str] = set()
    sends: set[str] = set()
    receives: set[str] = set()
    for fact in document["facts"]:
        domain = fact["domain"]
        if domain == "symbols":
            attributes = fact["attributes"]
            symbols.append(
                (
                    attributes["line"],
                    fact["name"],
                    fact["kind"],
                    attributes["scope"],
                    attributes["signature"],
                )
            )
        elif domain == "calls":
            calls[fact["name"]] += 1
        elif domain == "method-calls":
            methods[fact["name"]] += 1
        elif domain == "imports":
            imports.add(fact["name"])
        elif domain == "exports":
            exports.add(fact["name"])
        elif domain == "messages":
            (sends if fact["kind"] == "send" else receives).add(fact["name"])
    return (
        sorted(set(symbols)),
        dict(sorted(calls.items())),
        dict(sorted(methods.items())),
        imports,
        exports,
        sends,
        receives,
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_js_scanner_reference.py SCANNER_CLI")
    executable = Path(sys.argv[1]).resolve()
    for name, language, source in CASES:
        suffix = "vue" if language == "vue" else "ts" if language == "typescript" else "js"
        command = [str(executable), language, f"src/{name}.{suffix}"]
        first = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        second = subprocess.run(
            command, input=source.encode(), capture_output=True, check=True
        )
        if first.stdout != second.stdout:
            raise AssertionError(f"{name}: provider bytes are not deterministic")
        native = project(json.loads(first.stdout))
        oracle_source = vue_script_text(source) if language == "vue" else source
        oracle = js_file_facts(
            f"src/{name}.{suffix}", "web", language, oracle_source, "0" * 64
        )
        wanted = (
            sorted(
                {
                    (
                        symbol.line,
                        symbol.name,
                        symbol.kind,
                        symbol.scope,
                        symbol.signature,
                    )
                    for symbol in oracle.symbols
                }
            ),
            oracle.call_counts,
            oracle.method_call_counts,
            oracle.imports,
            oracle.exports,
            oracle.sends,
            oracle.receives,
        )
        # The immutable Python implementation reports the right-hand local
        # binding for ``export default Widget``. ECMAScript's public name is
        # ``default``; retain the old result only as migration evidence.
        if name == "typed":
            wanted = (*wanted[:4], (wanted[4] - {"Widget"}) | {"default"}, *wanted[5:])
        if native != wanted:
            raise AssertionError(
                f"{name}: native {language} projection differs\n"
                f"native={native!r}\noracle={wanted!r}"
            )
    regression = subprocess.run(
        [str(executable), "javascript", "src/object-functions.js"],
        input=(
            "const User = { index: function(req) { return req }, "
            "named: function inner() { return 1 } };\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    regression_symbols = {
        fact["name"]
        for fact in json.loads(regression.stdout)["facts"]
        if fact["domain"] == "symbols"
    }
    if regression_symbols != {"User.index", "User.named", "inner"}:
        raise AssertionError(
            "object function properties produced false lexical symbols: "
            f"{sorted(regression_symbols)!r}"
        )
    initializer_regression = subprocess.run(
        [str(executable), "javascript", "src/direct-initializers.js"],
        input=(
            "const direct = (value) => value;\n"
            "const short = value => value;\n"
            "const asynchronous = async (value) => value;\n"
            "const expression = function(value) { return value };\n"
            "const nested = makeData(1000, () => randomString(10));\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    initializer_symbols = {
        fact["name"]
        for fact in json.loads(initializer_regression.stdout)["facts"]
        if fact["domain"] == "symbols"
    }
    expected_initializers = {"asynchronous", "direct", "expression", "short"}
    if initializer_symbols != expected_initializers:
        raise AssertionError(
            "lexical arrow discovery escaped the direct initializer: "
            f"{sorted(initializer_symbols)!r}"
        )
    anonymous_class_regression = subprocess.run(
        [str(executable), "javascript", "src/anonymous-class.js"],
        input=(
            "const Plain = class { run() { return 1 } };\n"
            "var AdamW = class extends Optimizer {\n"
            "  constructor(params) { this.params = params }\n"
            "  applyGradients(values) { return values }\n"
            "};\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    anonymous_symbols = {
        fact["name"]
        for fact in json.loads(anonymous_class_regression.stdout)["facts"]
        if fact["domain"] == "symbols"
    }
    if anonymous_symbols != {
        "AdamW",
        "AdamW.applyGradients",
        "AdamW.constructor",
        "Plain",
        "Plain.run",
    }:
        raise AssertionError(
            "anonymous class expression lost its assignment binding: "
            f"{sorted(anonymous_symbols)!r}"
        )
    nested_switch_regression = subprocess.run(
        [str(executable), "javascript", "src/nested-switch.js"],
        input=(
            "switch (token.type) {\n"
            "case 'punc':\n"
            "  switch (token.value) {\n"
            "  case '{': open(); break;\n"
            "  case '}': close(); break;\n"
            "  }\n"
            "  break;\n"
            "case 'name': named(); break;\n"
            "}\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    nested_switch_receives = {
        fact["name"]
        for fact in json.loads(nested_switch_regression.stdout)["facts"]
        if fact["domain"] == "messages" and fact["kind"] == "receive"
    }
    if nested_switch_receives != {"type:name", "type:punc", "value:{", "value:}"}:
        raise AssertionError(
            "nested switch cases leaked into the outer message selector: "
            f"{sorted(nested_switch_receives)!r}"
        )
    quoted_property_regression = subprocess.run(
        [str(executable), "javascript", "src/quoted-property.js"],
        input=(
            "const component = { watch: { 'output._messages': { "
            "handler() { return 1 } } } };\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    quoted_symbols = {
        fact["name"]
        for fact in json.loads(quoted_property_regression.stdout)["facts"]
        if fact["domain"] == "symbols"
    }
    if quoted_symbols != {"component.watch.'output._messages'.handler"}:
        raise AssertionError(
            "quoted object-property segment was flattened into qualification: "
            f"{sorted(quoted_symbols)!r}"
        )
    module_regression = subprocess.run(
        [str(executable), "javascript", "src/module-surface.js"],
        input=(
            "const api = {run}; module.exports = api;\n"
            "module.exports = require('./bridge.js');\n"
            "export default api; export * from './esm.js';\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    module_facts = json.loads(module_regression.stdout)["facts"]
    module_exports = {
        fact["name"] for fact in module_facts if fact["domain"] == "exports"
    }
    module_routes = {
        (fact["kind"], fact["name"])
        for fact in module_facts
        if fact["domain"] == "module-reexports"
    }
    if module_exports != {"default"} or module_routes != {
        ("commonjs-require", "./bridge.js"),
        ("esm-star", "./esm.js"),
    }:
        raise AssertionError(
            "lexical module fallback invented alias exports or lost routes: "
            f"exports={sorted(module_exports)!r} routes={sorted(module_routes)!r}"
        )
    type_reexport_regression = subprocess.run(
        [str(executable), "typescript", "src/type-reexports.ts"],
        input=(
            "export { type $RefinementCtx as RefinementCtx, type LocalType, "
            "value as alias, type as literalType } from './core.js';\n"
            "export { type TypeOnly, runtime };\n"
        ).encode(),
        capture_output=True,
        check=True,
    )
    type_reexport_facts = [
        fact
        for fact in json.loads(type_reexport_regression.stdout)["facts"]
        if fact["domain"] == "exports"
    ]
    type_reexports = {
        fact["name"]: fact["attributes"].get("origin_name")
        for fact in type_reexport_facts
        if "origin" in fact["attributes"]
    }
    type_local_exports = {
        fact["name"]
        for fact in type_reexport_facts
        if "origin" not in fact["attributes"]
    }
    if type_reexports != {
        "RefinementCtx": "$RefinementCtx",
        "LocalType": "LocalType",
        "alias": "value",
        "literalType": "type",
    } or type_local_exports != {"TypeOnly", "runtime"}:
        raise AssertionError(
            "type-only export modifiers were treated as binding names: "
            f"reexports={type_reexports!r} local={type_local_exports!r}"
        )
    print(f"native JS/TS/Vue scanner parity passed: {len(CASES)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
