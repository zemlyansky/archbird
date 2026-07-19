"""Deterministic CPython-AST provider for the native Archbird fact IR.

This module is a host adapter.  It parses Python with CPython, emits strict
occurrence-level provider facts, and makes no architecture or call-resolution
claims.  The language-neutral core owns merging, aggregation, and verification.
"""

from __future__ import annotations

import ast
from bisect import bisect_right
import builtins
import codecs
from collections import deque
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import struct
import symtable
import sys
import tokenize
import unicodedata
import warnings
from typing import Callable, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


_DOMAINS = (
    "calls",
    "decorators",
    "export-origins",
    "exports",
    "imported-name-groups",
    "imported-names",
    "imports",
    "method-calls",
    "module-bindings",
    "module-reexports",
    "name-uses",
    "reexport-candidates",
    "symbols",
)
_CONFIGURATION_BYTES = (
    b'{"contract":"archbird-python-ast-v3",'
    b'"provider":"archbird-python-ast","schema_version":1}'
)
_STDLIB = set(getattr(sys, "stdlib_module_names", ())) | {
    "__future__",
    "abc",
    "argparse",
    "ast",
    "collections",
    "dataclasses",
    "fnmatch",
    "hashlib",
    "json",
    "math",
    "os",
    "pathlib",
    "re",
    "subprocess",
    "sys",
    "tarfile",
    "typing",
    "unittest",
}


def _is_stdlib(module: str) -> bool:
    return module.lstrip(".").split(".", 1)[0] in _STDLIB


@lru_cache(maxsize=1)
def _implementation_sha256() -> str:
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def python_ast_implementation_sha256() -> str:
    """Return the exact implementation identity emitted by this provider."""

    return _implementation_sha256()


def _framed(digest: "hashlib._Hash", value: bytes) -> None:
    digest.update(struct.pack(">Q", len(value)))
    digest.update(value)


def _fact_id(
    prefix: "hashlib._Hash",
    domain: str,
    kind: str,
    start: int,
    end: int,
    key: str,
) -> str:
    digest = prefix.copy()
    for value in (domain, kind):
        _framed(digest, value.encode("utf-8"))
    digest.update(struct.pack(">Q", start))
    digest.update(struct.pack(">Q", end))
    _framed(digest, key.encode("utf-8"))
    return "f:" + digest.hexdigest()


def _line_starts(raw: bytes) -> Tuple[int, ...]:
    starts = [0]
    for index, value in enumerate(raw):
        if value == 0x0A:
            starts.append(index + 1)
    return tuple(starts)


def _absolute_span(node: ast.AST, starts: Sequence[int]) -> Tuple[int, int]:
    line = getattr(node, "lineno", None)
    column = getattr(node, "col_offset", None)
    end_line = getattr(node, "end_lineno", None)
    end_column = getattr(node, "end_col_offset", None)
    if not all(isinstance(value, int) for value in (line, column, end_line, end_column)):
        raise ValueError(f"AST node {type(node).__name__} has no complete source span")
    if line < 1 or end_line < 1 or line > len(starts) or end_line > len(starts):
        raise ValueError(f"AST node {type(node).__name__} has an invalid source line")
    return starts[line - 1] + column, starts[end_line - 1] + end_column


def _line_for_span(starts: Sequence[int], span: Tuple[int, int]) -> int:
    """Return the one-based physical line containing a normalized fact span."""

    return bisect_right(starts, span[0])


def _identifier_byte(value: int) -> bool:
    return (
        value >= 0x80
        or 0x30 <= value <= 0x39
        or 0x41 <= value <= 0x5A
        or value == 0x5F
        or 0x61 <= value <= 0x7A
    )


class _SourcePositions:
    def __init__(self, text: str, raw: bytes, starts: Sequence[int]) -> None:
        self._text = text
        self._raw = raw
        self._starts = starts
        self._tokens: Optional[List[Tuple[int, str, int, int]]] = None

    def _source_tokens(self) -> Sequence[Tuple[int, str, int, int]]:
        if self._tokens is not None:
            return self._tokens
        lines = self._text.splitlines(keepends=True)
        tokens: List[Tuple[int, str, int, int]] = []
        ignored = {
            tokenize.COMMENT,
            tokenize.DEDENT,
            tokenize.ENCODING,
            tokenize.ENDMARKER,
            tokenize.INDENT,
            tokenize.NEWLINE,
            tokenize.NL,
        }
        for token in tokenize.generate_tokens(io.StringIO(self._text).readline):
            if token.type in ignored:
                continue
            start_line, start_column = token.start
            end_line, end_column = token.end
            if not (
                1 <= start_line <= len(self._starts)
                and 1 <= end_line <= len(self._starts)
            ):
                continue
            start_text = lines[start_line - 1] if start_line <= len(lines) else ""
            end_text = lines[end_line - 1] if end_line <= len(lines) else ""
            start = self._starts[start_line - 1] + len(
                start_text[:start_column].encode("utf-8")
            )
            end = self._starts[end_line - 1] + len(
                end_text[:end_column].encode("utf-8")
            )
            tokens.append((token.type, token.string, start, end))
        self._tokens = tokens
        return tokens

    def import_aliases(self, node: ast.AST) -> Tuple[Tuple[int, int], ...]:
        """Return exact import-alias spans on every supported CPython.

        CPython 3.9 gives ``ast.alias`` nodes no positions.  Token evidence is
        used only for that missing child-node metadata and is validated against
        the parsed alias names; the enclosing import must still have an exact
        AST span.
        """

        names = getattr(node, "names", ())
        if all(
            all(
                isinstance(getattr(alias, attribute, None), int)
                for attribute in ("lineno", "col_offset", "end_lineno", "end_col_offset")
            )
            for alias in names
        ):
            return tuple(_absolute_span(alias, self._starts) for alias in names)
        start, end = _absolute_span(node, self._starts)
        selected = [
            token
            for token in self._source_tokens()
            if token[2] >= start and token[3] <= end
        ]
        import_index = next(
            (
                index
                for index, token in enumerate(selected)
                if token[0] == tokenize.NAME and token[1] == "import"
            ),
            None,
        )
        if import_index is None:
            raise ValueError("cannot locate import keyword in Python AST span")
        selected = selected[import_index + 1 :]
        if selected and selected[0][1] == "(":
            selected = selected[1:]
        if selected and selected[-1][1] == ")":
            selected = selected[:-1]
        groups: List[List[Tuple[int, str, int, int]]] = [[]]
        for token in selected:
            if token[1] == ",":
                if groups[-1]:
                    groups.append([])
                continue
            groups[-1].append(token)
        groups = [group for group in groups if group]
        if len(groups) != len(names):
            raise ValueError("cannot align Python import aliases with source tokens")
        spans: List[Tuple[int, int]] = []
        for alias, group in zip(names, groups):
            parts = [token[1] for token in group]
            try:
                as_index = parts.index("as")
            except ValueError:
                as_index = len(parts)
            imported = "".join(parts[:as_index])
            renamed = "".join(parts[as_index + 1 :]) if as_index < len(parts) else None
            if imported != alias.name or renamed != alias.asname:
                raise ValueError("Python import alias tokens disagree with AST")
            spans.append((group[0][2], group[-1][3]))
        return tuple(spans)

    def named(
        self,
        name: str,
        within: Tuple[int, int],
        *,
        reverse: bool = False,
    ) -> Tuple[int, int]:
        if reverse:
            end = within[1]
            start = end
            while start > within[0] and _identifier_byte(self._raw[start - 1]):
                start -= 1
            try:
                value = self._raw[start:end].decode("utf-8")
            except UnicodeDecodeError:
                value = ""
            if unicodedata.normalize("NFKC", value) == name:
                return start, end
            raise ValueError(
                f"cannot locate trailing Python name {name!r} in source span"
            )
        cursor = within[0]
        examined = 0
        while cursor < within[1] and examined < 8:
            while cursor < within[1] and not _identifier_byte(self._raw[cursor]):
                cursor += 1
            start = cursor
            while cursor < within[1] and _identifier_byte(self._raw[cursor]):
                cursor += 1
            if cursor == start:
                break
            examined += 1
            try:
                value = self._raw[start:cursor].decode("utf-8")
            except UnicodeDecodeError:
                continue
            if unicodedata.normalize("NFKC", value) == name:
                return start, cursor
        raise ValueError(f"cannot locate Python name token {name!r} in source span")


class _Facts:
    def __init__(
        self,
        project: str,
        path: str,
        analysis_source: bytes | None = None,
        source_span: Callable[[Tuple[int, int]], Tuple[int, int]] | None = None,
    ) -> None:
        self.project = project
        self.path = path
        self._analysis_source = analysis_source
        self._source_span = source_span or (lambda span: span)
        self.rows: List[Dict[str, object]] = []
        self._id_prefix = hashlib.sha256()
        for value in (project, path):
            _framed(self._id_prefix, value.encode("utf-8"))

    def add(
        self,
        domain: str,
        kind: str,
        span: Tuple[int, int],
        key: str,
        name: Optional[str] = None,
        attributes: Optional[Mapping[str, object]] = None,
    ) -> None:
        analysis_start, analysis_end = span
        normalized_attributes = dict(attributes) if attributes else {}
        if (
            name is not None
            and self._analysis_source is not None
            and analysis_start < analysis_end
        ):
            try:
                source_name = self._analysis_source[
                    analysis_start:analysis_end
                ].decode("utf-8")
            except UnicodeDecodeError:
                source_name = ""
            semantic_leaf = name.rsplit(".", 1)[-1]
            normalized_source_name = unicodedata.normalize("NFKC", source_name)
            if source_name and source_name != normalized_source_name and (
                normalized_source_name == name
                or normalized_source_name == semantic_leaf
            ):
                normalized_attributes["source_name"] = source_name
        start, end = self._source_span(span)
        row: Dict[str, object] = {
            "claim": "syntax-structure",
            "domain": domain,
            "id": _fact_id(self._id_prefix, domain, kind, start, end, key),
            "key": key,
            "kind": kind,
            "path": self.path,
            "project": self.project,
            "span": {"end": end, "start": start},
        }
        if start < end:
            row["correlation"] = "span"
        if name is not None:
            row["name"] = name
        if normalized_attributes:
            row["attributes"] = normalized_attributes
        self.rows.append(row)

    def source_offset(self, offset: int) -> int:
        return self._source_span((offset, offset))[0]


class _ScopeImportCollector(ast.NodeVisitor):
    """Collect import bindings owned by one lexical scope only."""

    def __init__(self) -> None:
        self.bindings: Dict[str, Tuple[str, str, str]] = {}
        self.ambiguous: set[str] = set()

    def _record(self, local: str, module: str, imported: str) -> None:
        value = (module, imported, local)
        previous = self.bindings.get(local)
        if previous is not None and previous != value:
            self.ambiguous.add(local)
        else:
            self.bindings[local] = value

    def visit_Import(self, node: ast.Import) -> None:
        for alias in node.names:
            local = alias.asname or alias.name.split(".", 1)[0]
            bound_module = alias.name if alias.asname else local
            self._record(local, bound_module, "")

    def visit_ImportFrom(self, node: ast.ImportFrom) -> None:
        module = "." * node.level + (node.module or "")
        for alias in node.names:
            if alias.name == "*":
                continue
            self._record(alias.asname or alias.name, module, alias.name)

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        del node

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        del node

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        del node

    def visit_Lambda(self, node: ast.Lambda) -> None:
        del node


def _attribute_chain(node: ast.Attribute) -> Optional[Tuple[str, Tuple[str, ...]]]:
    """Return one syntactic ``name.attr...`` chain without resolving it."""

    attributes: List[str] = []
    current: ast.expr = node
    while isinstance(current, ast.Attribute):
        attributes.append(current.attr)
        current = current.value
    if not isinstance(current, ast.Name):
        return None
    attributes.reverse()
    return current.id, tuple(attributes)


def _join_module(module: str, segments: Sequence[str]) -> str:
    if not segments:
        return module
    suffix = ".".join(segments)
    if not module or module.endswith("."):
        return module + suffix
    return module + "." + suffix


@dataclass(frozen=True)
class _ReceiverOrigin:
    """One bounded syntactic origin for a later local receiver call."""

    root: str
    callee_attributes: Tuple[str, ...]
    assignment_start: int
    assignment_end: int
    derivation: str


@dataclass(frozen=True)
class _ReceiverScope:
    origins: Mapping[str, _ReceiverOrigin]
    written: frozenset[str]


def _receiver_expression_origin(node: ast.expr) -> Optional[Tuple[str, Tuple[str, ...], str]]:
    """Trace a call chain to its imported-looking syntactic root.

    This does not claim that a factory or fluent call preserves the receiver
    type.  It only retains a bounded candidate which the core must resolve to
    an actual mapped class member before exposing it.
    """

    if not isinstance(node, ast.Call):
        return None
    if isinstance(node.func, ast.Name):
        return node.func.id, (), "constructor-call"
    if not isinstance(node.func, ast.Attribute):
        return None
    nested = _receiver_expression_origin(node.func.value)
    if nested is not None:
        root, attributes, _derivation = nested
        return root, attributes, "call-chain"
    chain = _attribute_chain(node.func)
    if chain is None:
        return None
    root, attributes = chain
    return root, attributes, "imported-call"


class _ReceiverScopeCollector(_ScopeImportCollector):
    """Find names with exactly one syntactically traceable assignment.

    Multiple writes, deletion, destructuring, loop targets, imports, and
    rebinding deliberately remove the candidate.  Nested lexical scopes are
    collected separately by the main provider visitor.
    """

    def __init__(self, starts: Sequence[int]) -> None:
        super().__init__()
        self.starts = starts
        self.writes: Dict[str, List[Optional[_ReceiverOrigin]]] = {}

    def _write(self, name: str, origin: Optional[_ReceiverOrigin] = None) -> None:
        self.writes.setdefault(name, []).append(origin)

    def _write_target(
        self, target: ast.expr, origin: Optional[_ReceiverOrigin] = None
    ) -> None:
        if isinstance(target, ast.Name):
            self._write(target.id, origin)
            return
        for child in ast.walk(target):
            if isinstance(child, ast.Name) and isinstance(
                child.ctx, (ast.Store, ast.Del)
            ):
                self._write(child.id)

    def _origin(self, node: ast.expr) -> Optional[_ReceiverOrigin]:
        resolved = _receiver_expression_origin(node)
        if resolved is None:
            return None
        root, attributes, derivation = resolved
        start, end = _absolute_span(node, self.starts)
        return _ReceiverOrigin(root, attributes, start, end, derivation)

    def visit_Assign(self, node: ast.Assign) -> None:
        origin = self._origin(node.value) if len(node.targets) == 1 else None
        for target in node.targets:
            self._write_target(target, origin)
        self.visit(node.value)

    def visit_AnnAssign(self, node: ast.AnnAssign) -> None:
        origin = self._origin(node.value) if node.value is not None else None
        self._write_target(node.target, origin)
        self.visit(node.annotation)
        if node.value is not None:
            self.visit(node.value)

    def visit_AugAssign(self, node: ast.AugAssign) -> None:
        self._write_target(node.target)
        self.visit(node.value)

    def visit_NamedExpr(self, node: ast.NamedExpr) -> None:
        self._write_target(node.target, self._origin(node.value))
        self.visit(node.value)

    def visit_Import(self, node: ast.Import) -> None:
        super().visit_Import(node)
        for alias in node.names:
            self._write(alias.asname or alias.name.split(".", 1)[0])

    def visit_ImportFrom(self, node: ast.ImportFrom) -> None:
        super().visit_ImportFrom(node)
        for alias in node.names:
            if alias.name != "*":
                self._write(alias.asname or alias.name)

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        self._write(node.name)

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self._write(node.name)

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        self._write(node.name)

    def visit_Lambda(self, node: ast.Lambda) -> None:
        del node

    def result(self) -> _ReceiverScope:
        origins = {
            name: rows[0]
            for name, rows in self.writes.items()
            if len(rows) == 1 and rows[0] is not None
        }
        return _ReceiverScope(origins=origins, written=frozenset(self.writes))


def _scope_state(
    statements: Sequence[ast.stmt], starts: Sequence[int]
) -> Tuple[Dict[str, Tuple[str, str, str]], _ReceiverScope]:
    collector = _ReceiverScopeCollector(starts)
    for statement in statements:
        collector.visit(statement)
    imports = {
        name: value
        for name, value in collector.bindings.items()
        if name not in collector.ambiguous
    }
    return imports, collector.result()


class _PythonProviderVisitor(ast.NodeVisitor):
    def __init__(
        self,
        facts: _Facts,
        starts: Sequence[int],
        tokens: _SourcePositions,
        table: symtable.SymbolTable,
        tree: ast.Module,
    ) -> None:
        self.facts = facts
        self.starts = starts
        self.tokens = tokens
        self.scope: List[Tuple[str, str]] = []
        self.tables: List[Optional[symtable.SymbolTable]] = [table]
        imports, receivers = _scope_state(tree.body, starts)
        self.imports: List[Dict[str, Tuple[str, str, str]]] = [imports]
        self.receivers: List[_ReceiverScope] = [receivers]
        self.module_table = table
        self.child_tables: Dict[
            symtable.SymbolTable,
            Dict[Tuple[str, str, int], deque[symtable.SymbolTable]],
        ] = {}

    def _child_table(
        self, kind: str, name: str, line: int
    ) -> Optional[symtable.SymbolTable]:
        parent = self.tables[-1]
        if parent is None:
            return None
        index = self.child_tables.get(parent)
        if index is None:
            index = {}
            for child in parent.get_children():
                key = (child.get_type(), child.get_name(), child.get_lineno())
                index.setdefault(key, deque()).append(child)
            self.child_tables[parent] = index
        matches = index.get((kind, name, line))
        return matches.popleft() if matches else None

    @staticmethod
    def _bound_in_table(table: symtable.SymbolTable, name: str) -> bool:
        try:
            symbol = table.lookup(name)
        except KeyError:
            return False
        return bool(
            symbol.is_assigned()
            or symbol.is_imported()
            or symbol.is_namespace()
            or symbol.is_parameter()
        )

    def _binding(self, name: str) -> str:
        table = self.tables[-1]
        if table is None:
            return "unknown"
        try:
            symbol = table.lookup(name)
        except KeyError:
            return "builtin" if hasattr(builtins, name) else "unknown"
        # A module import referenced from a nested scope is represented by
        # symtable as a global in that nested table.  Resolve through the
        # lexical import tables before classifying generic project bindings.
        if self._import_binding(name) is not None:
            return "imported"
        if table.get_type() == "module":
            if self._bound_in_table(table, name):
                return "project"
            return "builtin" if hasattr(builtins, name) else "unknown"
        if (
            symbol.is_parameter()
            or symbol.is_imported()
            or symbol.is_local()
            or symbol.is_free()
            or symbol.is_nonlocal()
        ):
            return "local"
        if symbol.is_global():
            if self._bound_in_table(self.module_table, name):
                return "project"
            return "builtin" if hasattr(builtins, name) else "unknown"
        return "unknown"

    def _import_binding(self, name: str) -> Optional[Tuple[str, str, str]]:
        table = self.tables[-1]
        if table is None:
            return None
        try:
            symbol = table.lookup(name)
        except KeyError:
            return None
        if symbol.is_imported() and not symbol.is_assigned():
            return self.imports[-1].get(name)
        if symbol.is_global():
            try:
                module_symbol = self.module_table.lookup(name)
            except KeyError:
                return None
            if module_symbol.is_imported() and not module_symbol.is_assigned():
                return self.imports[0].get(name)
            return None
        if symbol.is_free() or symbol.is_nonlocal():
            for outer_table, imports in zip(
                reversed(self.tables[:-1]), reversed(self.imports[:-1])
            ):
                if outer_table is None:
                    continue
                try:
                    outer_symbol = outer_table.lookup(name)
                except KeyError:
                    continue
                if outer_symbol.is_imported() and not outer_symbol.is_assigned():
                    return imports.get(name)
        return None

    def _receiver_binding(
        self, name: str, before: int
    ) -> Optional[_ReceiverOrigin]:
        for scope in reversed(self.receivers):
            if name not in scope.written:
                continue
            origin = scope.origins.get(name)
            if origin is not None and origin.assignment_end <= before:
                return origin
            return None
        return None

    def _record_receiver_call(
        self,
        node: ast.Call,
        receiver: ast.expr,
        target_symbol: str,
        full_span: Tuple[int, int],
    ) -> None:
        origin: Optional[_ReceiverOrigin] = None
        receiver_name = "computed"
        if isinstance(receiver, ast.Name):
            receiver_name = receiver.id
            origin = self._receiver_binding(receiver.id, full_span[0])
        if origin is None:
            resolved = _receiver_expression_origin(receiver)
            if resolved is None:
                return
            root, attributes, derivation = resolved
            start, end = _absolute_span(receiver, self.starts)
            origin = _ReceiverOrigin(root, attributes, start, end, derivation)
        binding = self._import_binding(origin.root)
        if binding is None:
            return
        module, imported, local = binding
        # ``from pkg import Class`` roots name the owner class themselves;
        # attributes on their originating call are factory/method names.  For
        # ``import pkg; pkg.Class()`` the final attribute is the owner class.
        module_steps = (
            [imported, *origin.callee_attributes[:-1]]
            if imported and origin.callee_attributes
            else (
                [imported]
                if imported
                else list(origin.callee_attributes)
            )
        )
        if not module_steps:
            return
        key = ":".join(
            (
                str(len(module.encode("utf-8"))),
                module,
                ".".join(module_steps),
                target_symbol,
                local,
                receiver_name,
                str(self.facts.source_offset(origin.assignment_start)),
                str(self.facts.source_offset(origin.assignment_end)),
            )
        )
        self.facts.add(
            "name-uses",
            "inferred-receiver-call",
            full_span,
            key,
            target_symbol,
            {
                "enclosing": self._qualname("").rstrip("."),
                "imported": target_symbol,
                "line": node.func.lineno,
                "local": local,
                "module": _join_module(module, module_steps),
                "module_steps": module_steps,
                "receiver": receiver_name,
                "receiver_derivation": origin.derivation,
                "receiver_origin": origin.root,
                "receiver_resolution": "candidate",
                "root_module": module,
            },
        )

    def _visit_function_signature_parts(self, node: ast.AST) -> None:
        """Visit signature expressions in their lexical, not execution, owner.

        Defaults and annotations execute in the defining scope, so the active
        symbol table must remain the outer table.  They are nevertheless
        syntax children of the function definition.  Keeping ``scope`` and
        ``tables`` independent lets ``enclosing`` agree with portable syntax
        providers while ``binding`` retains CPython's evaluation semantics.
        """

        args = getattr(node, "args", None)
        if args is not None:
            for default in args.defaults:
                self.visit(default)
            for default in args.kw_defaults:
                if default is not None:
                    self.visit(default)
            for argument in [
                *getattr(args, "posonlyargs", ()),
                *args.args,
                *args.kwonlyargs,
            ]:
                if argument.annotation is not None:
                    self.visit(argument.annotation)
            if args.vararg is not None and args.vararg.annotation is not None:
                self.visit(args.vararg.annotation)
            if args.kwarg is not None and args.kwarg.annotation is not None:
                self.visit(args.kwarg.annotation)
        returns = getattr(node, "returns", None)
        if returns is not None:
            self.visit(returns)

    def _qualname(self, name: str) -> str:
        return ".".join([item[0] for item in self.scope] + [name])

    def _record_decorators(self, node: ast.AST, decorated: str) -> None:
        """Record syntax-level decoration without claiming callable dispatch."""

        for decorator in getattr(node, "decorator_list", ()):
            reference = decorator.func if isinstance(decorator, ast.Call) else decorator
            if isinstance(reference, ast.Name):
                root, attributes = reference.id, ()
            elif isinstance(reference, ast.Attribute):
                chain = _attribute_chain(reference)
                if chain is None:
                    continue
                root, attributes = chain
            else:
                continue
            span = _absolute_span(decorator, self.starts)
            rendered = ast.unparse(reference)
            common = {
                "application": "factory" if isinstance(decorator, ast.Call) else "direct",
                "decorated": decorated,
                "enclosing": self._qualname("").rstrip("."),
                "line": decorator.lineno,
                "root": root,
            }
            self.facts.add(
                "decorators",
                "decorated-by",
                span,
                f"{len(decorated.encode('utf-8'))}:{decorated}:{rendered}",
                rendered,
                common,
            )
            binding = self._import_binding(root)
            if binding is None:
                continue
            module, imported, local = binding
            if attributes:
                module_steps = [*([imported] if imported else []), *attributes[:-1]]
                target_symbol = attributes[-1]
            elif imported:
                module_steps = []
                target_symbol = imported
            else:
                # ``import callable_module`` does not establish which symbol,
                # if any, implements module callability.
                continue
            attributes_row = {
                **common,
                "imported": target_symbol,
                "local": local,
                "module": _join_module(module, module_steps),
                "module_steps": module_steps,
                "root_module": module,
            }
            key = ":".join(
                (
                    str(len(module.encode("utf-8"))),
                    module,
                    ".".join(module_steps),
                    target_symbol,
                    local,
                    decorated,
                )
            )
            self.facts.add(
                "name-uses",
                "decorator-reference",
                span,
                key,
                target_symbol,
                attributes_row,
            )

    def _record_module_variable(
        self, target: ast.expr, value: ast.expr | None
    ) -> None:
        if (
            self.scope
            or not isinstance(target, ast.Name)
            or target.id == "__all__"
            or isinstance(value, ast.Name)
        ):
            return
        span = _absolute_span(target, self.starts)
        self.facts.add(
            "symbols",
            "variable",
            span,
            target.id,
            target.id,
            {
                "line": target.lineno,
                "scope": "module",
                "signature": "",
            },
        )

    def visit_Assign(self, node: ast.Assign) -> None:
        for target in node.targets:
            self._record_module_variable(target, node.value)
        self.visit(node.value)

    def visit_AnnAssign(self, node: ast.AnnAssign) -> None:
        self._record_module_variable(node.target, node.value)
        self.visit(node.annotation)
        if node.value is not None:
            self.visit(node.value)

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        node_span = _absolute_span(node, self.starts)
        name_span = self.tokens.named(node.name, node_span)
        bases = ", ".join(ast.unparse(base) for base in node.bases)
        signature = f"class {node.name}({bases})" if bases else f"class {node.name}"
        qualified = self._qualname(node.name)
        self.facts.add(
            "symbols",
            "class",
            name_span,
            qualified,
            qualified,
            {"line": node.lineno, "scope": "class", "signature": signature},
        )
        self._record_decorators(node, qualified)
        for decorator in node.decorator_list:
            self.visit(decorator)
        self.scope.append((node.name, "class"))
        for base in node.bases:
            self.visit(base)
        for keyword in node.keywords:
            self.visit(keyword.value)
        self.scope.pop()
        self.scope.append((node.name, "class"))
        self.tables.append(self._child_table("class", node.name, node.lineno))
        imports, receivers = _scope_state(node.body, self.starts)
        self.imports.append(imports)
        self.receivers.append(receivers)
        for statement in node.body:
            self.visit(statement)
        self.receivers.pop()
        self.imports.pop()
        self.tables.pop()
        self.scope.pop()

    def _visit_function(
        self, node: ast.AST, name: str, args: ast.arguments, async_: bool
    ) -> None:
        node_span = _absolute_span(node, self.starts)
        name_span = self.tokens.named(name, node_span)
        in_class = any(kind == "class" for _, kind in self.scope)
        kind = "method" if in_class else "function"
        prefix = "async def" if async_ else "def"
        signature = f"{prefix} {name}({ast.unparse(args)})"
        returns = getattr(node, "returns", None)
        if returns is not None:
            signature += f" -> {ast.unparse(returns)}"
        qualified = self._qualname(name)
        self.facts.add(
            "symbols",
            kind,
            name_span,
            qualified,
            qualified,
            {
                "line": getattr(node, "lineno"),
                "scope": kind,
                "signature": signature,
            },
        )
        self._record_decorators(node, qualified)
        for decorator in getattr(node, "decorator_list", ()):
            self.visit(decorator)
        self.scope.append((name, "function"))
        self._visit_function_signature_parts(node)
        self.scope.pop()
        self.scope.append((name, "function"))
        self.tables.append(self._child_table("function", name, node.lineno))
        imports, receivers = _scope_state(getattr(node, "body"), self.starts)
        self.imports.append(imports)
        self.receivers.append(receivers)
        for statement in getattr(node, "body"):
            self.visit(statement)
        self.receivers.pop()
        self.imports.pop()
        self.tables.pop()
        self.scope.pop()

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        self._visit_function(node, node.name, node.args, False)

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self._visit_function(node, node.name, node.args, True)

    def visit_Call(self, node: ast.Call) -> None:
        visit_callee = True
        if isinstance(node.func, ast.Name):
            span = _absolute_span(node.func, self.starts)
            self.facts.add(
                "calls",
                "free-call",
                span,
                node.func.id,
                node.func.id,
                {
                    "binding": self._binding(node.func.id),
                    "enclosing": self._qualname("").rstrip("."),
                    "line": node.func.lineno,
                },
            )
            binding = self._import_binding(node.func.id)
            if binding is not None:
                module, imported, local = binding
                key = (
                    f"{len(module.encode('utf-8'))}:{module}:"
                    f"{imported}:{local}:call"
                )
                self.facts.add(
                    "name-uses",
                    "imported-name-call",
                    span,
                    key,
                    local,
                    {
                        "enclosing": self._qualname("").rstrip("."),
                        "imported": imported,
                        "line": node.func.lineno,
                        "local": local,
                        "module": module,
                    },
                )
            # The call fact above is the complete syntax-level claim for this
            # occurrence.  Visiting the same Name again would emit a second,
            # weaker imported-name-use fact for the identical span.
            visit_callee = False
        elif isinstance(node.func, ast.Attribute):
            full_span = _absolute_span(node.func, self.starts)
            span = self.tokens.named(node.func.attr, full_span, reverse=True)
            self.facts.add(
                "method-calls",
                "method-call",
                span,
                node.func.attr,
                node.func.attr,
                {
                    "enclosing": self._qualname("").rstrip("."),
                    "line": _line_for_span(self.starts, span),
                },
            )
            chain = _attribute_chain(node.func)
            if chain is not None:
                root, attributes = chain
                binding = self._import_binding(root)
                if binding is not None and attributes:
                    module, imported, local = binding
                    module_steps = [
                        *([imported] if imported else []),
                        *attributes[:-1],
                    ]
                    target_symbol = attributes[-1]
                    target_module = _join_module(module, module_steps)
                    key = ":".join(
                        (
                            str(len(module.encode("utf-8"))),
                            module,
                            ".".join(module_steps),
                            target_symbol,
                            local,
                        )
                    )
                    self.facts.add(
                        "name-uses",
                        "imported-attribute-call",
                        full_span,
                        key,
                        target_symbol,
                        {
                            "enclosing": self._qualname("").rstrip("."),
                            "imported": target_symbol,
                            "line": node.func.lineno,
                            "local": local,
                            "module": target_module,
                            "module_steps": module_steps,
                            "root_module": module,
                            },
                        )
                elif attributes:
                    self._record_receiver_call(
                        node, node.func.value, attributes[-1], full_span
                    )
            else:
                self._record_receiver_call(
                    node, node.func.value, node.func.attr, full_span
                )
            # A simple name.attribute chain is represented by the attribute
            # call fact.  For a computed receiver, retain nested evidence such
            # as ``factory().method()`` by visiting the receiver expression.
            if chain is not None:
                visit_callee = False
            else:
                self.visit(node.func.value)
                visit_callee = False
        if visit_callee:
            self.visit(node.func)
        for argument in node.args:
            self.visit(argument)
        for keyword in node.keywords:
            self.visit(keyword.value)

    def visit_Name(self, node: ast.Name) -> None:
        if not isinstance(node.ctx, ast.Load):
            return
        binding = self._import_binding(node.id)
        if binding is None:
            return
        module, imported, local = binding
        span = _absolute_span(node, self.starts)
        kind = "imported-name-use" if imported else "imported-module-use"
        key = f"{len(module.encode('utf-8'))}:{module}:{imported}:{local}"
        self.facts.add(
            "name-uses",
            kind,
            span,
            key,
            local,
            {
                "enclosing": self._qualname("").rstrip("."),
                "imported": imported,
                "line": node.lineno,
                "local": local,
                "module": module,
            },
        )

    def visit_Lambda(self, node: ast.Lambda) -> None:
        for default in node.args.defaults:
            self.visit(default)
        for default in node.args.kw_defaults:
            if default is not None:
                self.visit(default)
        self.tables.append(self._child_table("function", "lambda", node.lineno))
        self.imports.append({})
        self.visit(node.body)
        self.imports.pop()
        self.tables.pop()

    def _visit_comprehension(
        self, node: ast.AST, name: str, values: Sequence[ast.AST]
    ) -> None:
        generators = getattr(node, "generators")
        if generators:
            self.visit(generators[0].iter)
        self.tables.append(self._child_table("function", name, node.lineno))
        self.imports.append({})
        for index, generator in enumerate(generators):
            if index:
                self.visit(generator.iter)
            self.visit(generator.target)
            for condition in generator.ifs:
                self.visit(condition)
        for value in values:
            self.visit(value)
        self.imports.pop()
        self.tables.pop()

    def visit_ListComp(self, node: ast.ListComp) -> None:
        self._visit_comprehension(node, "listcomp", (node.elt,))

    def visit_SetComp(self, node: ast.SetComp) -> None:
        self._visit_comprehension(node, "setcomp", (node.elt,))

    def visit_GeneratorExp(self, node: ast.GeneratorExp) -> None:
        self._visit_comprehension(node, "genexpr", (node.elt,))

    def visit_DictComp(self, node: ast.DictComp) -> None:
        self._visit_comprehension(node, "dictcomp", (node.key, node.value))

    def visit_Import(self, node: ast.Import) -> None:
        for alias, span in zip(node.names, self.tokens.import_aliases(node)):
            local = alias.asname or alias.name.split(".", 1)[0]
            bound_module = alias.name if alias.asname else local
            self.facts.add(
                "imports",
                "module",
                span,
                alias.name,
                alias.name,
                {"stdlib": _is_stdlib(alias.name)},
            )
            self.facts.add(
                "module-bindings",
                "module",
                span,
                f"{len(local.encode('utf-8'))}:{local}:{bound_module}",
                local,
                {"target_module": bound_module},
            )

    def visit_ImportFrom(self, node: ast.ImportFrom) -> None:
        prefix = "." * node.level + (node.module or "")
        if not prefix:
            return
        node_span = _absolute_span(node, self.starts)
        self.facts.add(
            "imports",
            "module",
            node_span,
            prefix,
            prefix,
            {"stdlib": _is_stdlib(prefix)},
        )
        self.facts.add(
            "imported-name-groups", "module", node_span, prefix, prefix
        )
        for alias, span in zip(node.names, self.tokens.import_aliases(node)):
            if alias.name == "*":
                self.facts.add(
                    "module-reexports",
                    "python-star",
                    node_span,
                    prefix,
                    prefix,
                )
                continue
            self.facts.add(
                "imported-names",
                "member",
                span,
                f"{len(prefix.encode('utf-8'))}:{prefix}{alias.name}",
                alias.name,
                {"module": prefix},
            )
            if node.module is None:
                local = alias.asname or alias.name
                target_module = _join_module(prefix, (alias.name,))
                self.facts.add(
                    "module-bindings",
                    "module",
                    span,
                    f"{len(local.encode('utf-8'))}:{local}:{target_module}",
                    local,
                    {"target_module": target_module},
                )


def _top_level_exports(
    tree: ast.Module,
    path: str,
    starts: Sequence[int],
    tokens: _SourcePositions,
) -> Tuple[
    Sequence[str],
    Mapping[str, str],
    Mapping[str, str],
    Mapping[str, Tuple[int, int]],
    Sequence[Tuple[str, Tuple[int, int]]],
]:
    explicit: Optional[List[str]] = None
    explicit_spans: Dict[str, Tuple[int, int]] = {}
    imported: List[str] = []
    defined: List[str] = []
    origins: Dict[str, str] = {}
    origin_names: Dict[str, str] = {}
    spans: Dict[str, Tuple[int, int]] = {}
    imported_occurrences: List[Tuple[str, Tuple[int, int]]] = []
    for node in tree.body:
        if isinstance(node, ast.ImportFrom):
            prefix = "." * node.level + (node.module or "")
            for alias, span in zip(node.names, tokens.import_aliases(node)):
                local_name = alias.asname or alias.name
                if local_name == "*":
                    continue
                imported.append(local_name)
                imported_occurrences.append((local_name, span))
                origins[local_name] = prefix
                origin_names[local_name] = alias.name
                spans[local_name] = span
        elif isinstance(node, ast.Import):
            for alias, span in zip(node.names, tokens.import_aliases(node)):
                local_name = alias.asname or alias.name.split(".")[0]
                imported.append(local_name)
                imported_occurrences.append((local_name, span))
                origins[local_name] = alias.name
                origin_names[local_name] = ""
                spans[local_name] = span
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            node_span = _absolute_span(node, starts)
            span = tokens.named(node.name, node_span)
            defined.append(node.name)
            origins[node.name] = ""
            origin_names[node.name] = node.name
            spans[node.name] = span
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            value = node.value
            if any(
                isinstance(target, ast.Name) and target.id == "__all__"
                for target in targets
            ):
                try:
                    candidate = ast.literal_eval(value)
                except (ValueError, TypeError):
                    candidate = None
                if isinstance(candidate, (list, tuple)) and all(
                    isinstance(item, str) for item in candidate
                ):
                    explicit = list(candidate)
                    if isinstance(value, (ast.List, ast.Tuple)):
                        for item in value.elts:
                            if isinstance(item, ast.Constant) and isinstance(
                                item.value, str
                            ):
                                explicit_spans.setdefault(
                                    item.value, _absolute_span(item, starts)
                                )
                continue
            for target in targets:
                if not isinstance(target, ast.Name):
                    continue
                defined.append(target.id)
                origins[target.id] = (
                    origins.get(value.id, "") if isinstance(value, ast.Name) else ""
                )
                origin_names[target.id] = (
                    origin_names.get(value.id, value.id)
                    if isinstance(value, ast.Name)
                    else ""
                )
                spans[target.id] = _absolute_span(target, starts)
    if explicit is not None:
        selected = explicit
        fallback = _absolute_span(tree.body[0], starts) if tree.body else (0, 0)
        selected_spans = {
            name: explicit_spans.get(name, spans.get(name, fallback))
            for name in selected
        }
    elif PurePosixPath(path).name == "__init__.py":
        selected = imported + defined
        selected_spans = spans
    else:
        selected = defined
        selected_spans = spans
    return selected, origins, origin_names, selected_spans, imported_occurrences


def _syntax_error_span(
    error: SyntaxError, text: str, raw: bytes, starts: Sequence[int]
) -> Optional[Tuple[int, int]]:
    if not isinstance(error.lineno, int) or not isinstance(error.offset, int):
        return None
    if error.lineno < 1 or error.lineno > len(starts):
        return None
    lines = text.split("\n")
    line = lines[error.lineno - 1] if error.lineno <= len(lines) else ""
    column = max(0, error.offset - 1)
    start = starts[error.lineno - 1] + len(line[:column].encode("utf-8"))
    return min(start, len(raw)), min(start, len(raw))


class _SourceOffsets:
    """Translate decoded UTF-8 parser offsets to exact source-byte offsets."""

    def __init__(self, text: str, source: bytes, encoding: str) -> None:
        encoder = codecs.getincrementalencoder(encoding)()
        encoded = bytearray(encoder.encode("", final=False))
        utf8_offset = 0
        source_offset = len(encoded)
        offsets = {0: source_offset}
        for character in text:
            chunk = encoder.encode(character, final=False)
            encoded.extend(chunk)
            utf8_offset += len(character.encode("utf-8"))
            source_offset += len(chunk)
            offsets[utf8_offset] = source_offset
        encoded.extend(encoder.encode("", final=True))
        if bytes(encoded) != source:
            raise UnicodeError(
                f"Python source encoding {encoding!r} did not round-trip exact bytes"
            )
        self._offsets = offsets

    def span(self, span: Tuple[int, int]) -> Tuple[int, int]:
        try:
            return self._offsets[span[0]], self._offsets[span[1]]
        except KeyError as error:
            raise ValueError(
                "CPython reported a source position inside an encoded character"
            ) from error


def _decode_python_source(source: bytes) -> Tuple[str, _SourceOffsets | None]:
    encoding, _ = tokenize.detect_encoding(io.BytesIO(source).readline)
    text = source.decode(encoding)
    if encoding == "utf-8":
        return text, None
    return text, _SourceOffsets(text, source, encoding)


def python_ast_provider_facts(
    *,
    project: str,
    path: str,
    text: str | None = None,
    source_bytes: bytes | None = None,
    source_manifest_sha256: str | None = None,
) -> bytes:
    """Return compact canonical provider facts bound to one Python source.

    ``source_manifest_sha256`` is accepted for source compatibility with the
    pre-incremental provider API but no longer enters the artifact. File-local
    evidence binds to the exact path and source digest instead of becoming
    stale whenever an unrelated repository file changes.
    """

    if (text is None) == (source_bytes is None):
        raise ValueError("exactly one of text or source_bytes must be supplied")
    if source_bytes is None:
        assert text is not None
        source = text.encode("utf-8")
    else:
        source = source_bytes
    encoding_error: UnicodeError | SyntaxError | None = None
    source_offsets: _SourceOffsets | None = None
    if source_bytes is not None:
        try:
            text, source_offsets = _decode_python_source(source)
        except (UnicodeError, SyntaxError) as error:
            encoding_error = error
            text = ""
    assert text is not None
    raw = text.encode("utf-8")
    starts = _line_starts(raw)
    capabilities = [
        {
            "boundary": (
                "complete for CPython AST occurrences in this file; no dynamic "
                "name or target resolution"
            ),
            "claims": ["syntax-structure"],
            "coverage": "complete" if domain not in {"exports", "reexport-candidates"} else "bounded",
            "domain": domain,
        }
        for domain in _DOMAINS
    ]
    facts = _Facts(
        project,
        path,
        raw,
        source_offsets.span if source_offsets is not None else None,
    )
    diagnostics: List[Dict[str, object]] = []
    if encoding_error is not None:
        for capability in capabilities:
            capability["coverage"] = "none"
            capability["boundary"] = (
                "inapplicable: source bytes were not accepted by Python source "
                "encoding detection; portable or semantic providers may still "
                "contribute evidence"
            )
        diagnostic: Dict[str, object] = {
            "code": "python-ast-encoding-inapplicable",
            "message": (
                "CPython AST provider could not decode this source: "
                f"{encoding_error}"
            ),
            "path": path,
            "project": project,
            "severity": "note",
        }
        if isinstance(encoding_error, UnicodeDecodeError):
            diagnostic["span"] = {
                "end": encoding_error.end,
                "start": encoding_error.start,
            }
        diagnostics.append(diagnostic)
    else:
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", SyntaxWarning)
                tree = ast.parse(text, filename=path)
                table = symtable.symtable(text, path, "exec")
            source_tokens = _SourcePositions(text, raw, starts)
            visitor = _PythonProviderVisitor(
                facts, starts, source_tokens, table, tree
            )
            visitor.visit(tree)
            selected, origins, origin_names, export_spans, imported = (
                _top_level_exports(tree, path, starts, source_tokens)
            )
            for name, span in imported:
                if name and not name.startswith("_"):
                    facts.add(
                        "reexport-candidates",
                        "name",
                        span,
                        name,
                        name,
                    )
            for name in selected:
                if not name or name.startswith("_"):
                    continue
                span = export_spans.get(name, (0, 0))
                facts.add(
                    "exports",
                    "name",
                    span,
                    name,
                    name,
                    {
                        "origin": origins.get(name, ""),
                        "origin_name": origin_names.get(name, name),
                    },
                )
                facts.add(
                    "export-origins",
                    "origin",
                    span,
                    name,
                    name,
                    {
                        "origin": origins.get(name, ""),
                        "origin_name": origin_names.get(name, name),
                    },
                )
        except SyntaxError as error:
            for capability in capabilities:
                capability["coverage"] = "none"
                capability["boundary"] = (
                    "inapplicable: source was not accepted by "
                    f"CPython {sys.version_info.major}.{sys.version_info.minor} "
                    "syntax; portable or semantic providers may still "
                    "contribute evidence"
                )
            diagnostic = {
                "code": "python-ast-inapplicable",
                "message": (
                    f"CPython {sys.version_info.major}.{sys.version_info.minor} AST "
                    f"provider did not accept this source: {error}"
                ),
                "path": path,
                "project": project,
                "severity": "note",
            }
            span = _syntax_error_span(error, text, raw, starts)
            if span is not None:
                if source_offsets is not None:
                    span = source_offsets.span(span)
                diagnostic["span"] = {"end": span[1], "start": span[0]}
            diagnostics.append(diagnostic)
    sorted_facts: List[Dict[str, object]] = []
    for row in sorted(facts.rows, key=lambda item: str(item["id"])):
        if sorted_facts and sorted_facts[-1]["id"] == row["id"]:
            if sorted_facts[-1] != row:
                raise ValueError(
                    "Python AST provider produced conflicting facts with one identity"
                )
            continue
        sorted_facts.append(row)
    document = {
        "artifact": "archbird-provider-facts",
        "capabilities": capabilities,
        "diagnostics": diagnostics,
        "facts": sorted_facts,
        "inputs": [
            {
                "path": path,
                "project": project,
                "source_sha256": hashlib.sha256(source).hexdigest(),
            }
        ],
        "producer": {
            "configuration_sha256": hashlib.sha256(
                _CONFIGURATION_BYTES
            ).hexdigest(),
            "implementation_sha256": _implementation_sha256(),
            "name": "archbird-python-ast",
            "runtime": f"cpython-{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
            "version": "3",
        },
        "provenance": "derived",
        "resolutions": [],
        "schema_version": 1,
        "subject": {"path": path, "project": project, "scope": "file"},
    }
    return json.dumps(
        document,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")


__all__ = [
    "python_ast_implementation_sha256",
    "python_ast_provider_facts",
]
