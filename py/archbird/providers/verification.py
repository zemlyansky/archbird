"""CPython AST precision provider for native verification extractors."""

from __future__ import annotations

import ast
import fnmatch
import hashlib
import json
from pathlib import Path
import platform
from typing import Any, Mapping


class _UnsupportedExpression(ValueError):
    pass


def _integer(node: ast.AST, env: Mapping[str, int]) -> int:
    if (
        isinstance(node, ast.Constant)
        and isinstance(node.value, int)
        and not isinstance(node.value, bool)
    ):
        return node.value
    if isinstance(node, ast.Name) and node.id in env:
        return env[node.id]
    if isinstance(node, ast.UnaryOp):
        value = _integer(node.operand, env)
        if isinstance(node.op, ast.UAdd):
            return value
        if isinstance(node.op, ast.USub):
            return -value
        if isinstance(node.op, ast.Invert):
            return ~value
    if isinstance(node, ast.BinOp):
        left, right = _integer(node.left, env), _integer(node.right, env)
        if isinstance(node.op, ast.Add):
            return left + right
        if isinstance(node.op, ast.Sub):
            return left - right
        if isinstance(node.op, ast.Mult):
            return left * right
        if isinstance(node.op, ast.FloorDiv):
            return left // right
        if isinstance(node.op, ast.Mod):
            return left % right
        if isinstance(node.op, ast.LShift):
            return left << right
        if isinstance(node.op, ast.RShift):
            return left >> right
        if isinstance(node.op, ast.BitOr):
            return left | right
        if isinstance(node.op, ast.BitAnd):
            return left & right
        if isinstance(node.op, ast.BitXor):
            return left ^ right
    raise _UnsupportedExpression(ast.dump(node, include_attributes=False))


def _member_name(node: ast.AST) -> str:
    if isinstance(node, ast.Attribute):
        return node.attr
    if isinstance(node, ast.Name):
        return node.id
    raise _UnsupportedExpression(ast.dump(node, include_attributes=False))


def _set_value(node: ast.AST, env: Mapping[str, set[str]]) -> set[str]:
    if isinstance(node, (ast.Set, ast.List, ast.Tuple)):
        return {_member_name(item) for item in node.elts}
    if isinstance(node, ast.Name) and node.id in env:
        return set(env[node.id])
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr):
        return _set_value(node.left, env) | _set_value(node.right, env)
    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr in {"union", "intersection", "difference"}
    ):
        rows = [_set_value(item, env) for item in node.args]
        if isinstance(node.func.value, ast.Name) and node.func.value.id != "set":
            rows.insert(0, _set_value(node.func.value, env))
        if not rows:
            return set()
        result = set(rows[0])
        for row in rows[1:]:
            if node.func.attr == "union":
                result |= row
            elif node.func.attr == "intersection":
                result &= row
            else:
                result -= row
        return result
    raise _UnsupportedExpression(ast.dump(node, include_attributes=False))


def _normalize(spec: Mapping[str, Any], value: str) -> str | None:
    result = value
    prefix = str(spec.get("strip_prefix", ""))
    suffix = str(spec.get("strip_suffix", ""))
    if prefix and result.startswith(prefix):
        result = result[len(prefix) :]
    if suffix and result.endswith(suffix):
        result = result[: -len(suffix)]
    include = tuple(str(item) for item in spec.get("include", ()))
    exclude = tuple(str(item) for item in spec.get("exclude", ()))
    if include and not any(fnmatch.fnmatchcase(result, item) for item in include):
        return None
    if any(fnmatch.fnmatchcase(result, item) for item in exclude):
        return None
    return result


def _evidence(
    project: str, path: str, line: int, sha256: str, detail: str
) -> dict[str, Any]:
    return {
        "provenance": "derived",
        "project": project,
        "path": path,
        "line": line,
        "sha256": sha256,
        "detail": detail,
    }


def _item(
    key: str,
    value: Any,
    evidence: Mapping[str, Any],
    *,
    state: str = "current",
    message: str = "",
) -> dict[str, Any]:
    return {
        "key": key,
        "label": key,
        "value": value,
        "attributes": {},
        "state": state,
        "message": message,
        "evidence": [dict(evidence)],
    }


def _unknown(
    name: str,
    shape: str,
    project: str,
    message: str,
    evidence: Mapping[str, Any],
) -> dict[str, Any]:
    return {
        "name": name,
        "shape": shape,
        "project": project,
        "state": "unknown",
        "message": message,
        "items": [
            _item("extractor", None, evidence, state="unknown", message=message)
        ],
    }


def _python_enum(
    name: str,
    spec: Mapping[str, Any],
    project: str,
    path: str,
    tree: ast.AST,
    source_sha256: str,
) -> dict[str, Any]:
    class_name = str(spec["class"])
    base = _evidence(
        project, path, 0, source_sha256, f"python enum {class_name}"
    )
    classes = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.ClassDef) and node.name == class_name
    ]
    if len(classes) != 1:
        return _unknown(
            name,
            "values",
            project,
            f"expected one class {class_name!r}, found {len(classes)}",
            base,
        )
    env: dict[str, int] = {}
    next_value = int(spec.get("auto_start", 1))
    items: list[dict[str, Any]] = []
    for statement in classes[0].body:
        target: ast.Name | None = None
        expression: ast.AST | None = None
        if (
            isinstance(statement, ast.Assign)
            and len(statement.targets) == 1
            and isinstance(statement.targets[0], ast.Name)
        ):
            target, expression = statement.targets[0], statement.value
        elif (
            isinstance(statement, ast.AnnAssign)
            and isinstance(statement.target, ast.Name)
            and statement.value is not None
        ):
            target, expression = statement.target, statement.value
        if target is None or expression is None or target.id.startswith("_"):
            continue
        state, message = "current", ""
        try:
            if (
                isinstance(expression, ast.Call)
                and isinstance(expression.func, ast.Name)
                and expression.func.id == "auto"
                and not expression.args
                and not expression.keywords
            ):
                value: Any = next_value
            else:
                value = _integer(expression, env)
            env[target.id] = value
            next_value = max(next_value, value + 1)
        except (ArithmeticError, _UnsupportedExpression) as error:
            value = (
                ast.unparse(expression)
                if hasattr(ast, "unparse")
                else ast.dump(expression)
            )
            state = "unknown"
            message = f"unsupported enum expression: {error}"
        normalized = _normalize(spec, target.id)
        if normalized is None:
            continue
        items.append(
            _item(
                normalized,
                value,
                _evidence(
                    project,
                    path,
                    int(statement.lineno),
                    source_sha256,
                    target.id,
                ),
                state=state,
                message=message,
            )
        )
    if not items:
        return _unknown(
            name,
            "values",
            project,
            f"class {class_name!r} has no selected enum members",
            base,
        )
    return {
        "name": name,
        "shape": "values",
        "project": project,
        "state": "current",
        "message": "",
        "items": sorted(items, key=lambda row: row["key"]),
    }


def _python_set(
    name: str,
    spec: Mapping[str, Any],
    project: str,
    path: str,
    tree: ast.AST,
    source_sha256: str,
) -> dict[str, Any]:
    class_name, attribute = str(spec["class"]), str(spec["attribute"])
    base = _evidence(
        project,
        path,
        0,
        source_sha256,
        f"python set {class_name}.{attribute}",
    )
    classes = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.ClassDef) and node.name == class_name
    ]
    if len(classes) != 1:
        return _unknown(
            name,
            "set",
            project,
            f"expected one class {class_name!r}, found {len(classes)}",
            base,
        )
    env: dict[str, set[str]] = {}
    selected: tuple[ast.AST, set[str]] | None = None
    for statement in classes[0].body:
        target: str | None = None
        expression: ast.AST | None = None
        if (
            isinstance(statement, ast.Assign)
            and len(statement.targets) == 1
            and isinstance(statement.targets[0], ast.Name)
        ):
            target, expression = statement.targets[0].id, statement.value
        elif (
            isinstance(statement, ast.AnnAssign)
            and isinstance(statement.target, ast.Name)
            and statement.value is not None
        ):
            target, expression = statement.target.id, statement.value
        if target is None or expression is None:
            continue
        try:
            value = _set_value(expression, env)
        except _UnsupportedExpression:
            if target == attribute:
                return _unknown(
                    name,
                    "set",
                    project,
                    f"unsupported set expression for {attribute!r}",
                    base,
                )
            continue
        env[target] = value
        if target == attribute:
            selected = (statement, value)
    if selected is None:
        return _unknown(
            name,
            "set",
            project,
            f"attribute {attribute!r} not found",
            base,
        )
    statement, values = selected
    items = []
    for value in sorted(values):
        normalized = _normalize(spec, value)
        if normalized is None:
            continue
        items.append(
            _item(
                normalized,
                None,
                _evidence(
                    project,
                    path,
                    int(getattr(statement, "lineno", 0)),
                    source_sha256,
                    f"{class_name}.{attribute}: {value}",
                ),
            )
        )
    return {
        "name": name,
        "shape": "set",
        "project": project,
        "state": "current",
        "message": "",
        "items": items,
    }


def python_verification_fact(
    *,
    name: str,
    spec: Mapping[str, Any],
    project: str,
    path: str,
    text: str,
) -> Mapping[str, Any]:
    """Produce one strict fact-set candidate; the C core revalidates it."""

    source_sha256 = hashlib.sha256(text.encode("utf-8")).hexdigest()
    try:
        tree = ast.parse(text, filename=path)
    except SyntaxError as error:
        kind = str(spec["kind"])
        shape = "values" if kind == "python_enum" else "set"
        fact = _unknown(
            name,
            shape,
            project,
            f"Python parse failed: {error}",
            _evidence(
                project,
                path,
                0,
                source_sha256,
                f"python {'enum' if kind == 'python_enum' else 'set'}",
            ),
        )
    else:
        if spec["kind"] == "python_enum":
            fact = _python_enum(name, spec, project, path, tree, source_sha256)
        elif spec["kind"] == "python_set":
            fact = _python_set(name, spec, project, path, tree, source_sha256)
        else:
            raise ValueError(f"unsupported Python verification extractor: {spec['kind']}")
    fact["producer"] = {
        "name": "archbird-python-verification-ast",
        "version": "1",
        "implementation_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "runtime": f"cpython-{platform.python_version()}",
    }
    return fact


def python_verification_fact_json(**kwargs: Any) -> bytes:
    return json.dumps(
        python_verification_fact(**kwargs),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


__all__ = ["python_verification_fact", "python_verification_fact_json"]
