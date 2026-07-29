#!/usr/bin/env python3
"""Adversarial tests for the optional ast-grep Plan adapter."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "py"))

from archbird.adapters.ast_grep import (
    AstGrepAdapterError,
    inspect_ast_grep_executable,
    materialize_ast_grep_operations,
)


FAKE = r'''
import json
from pathlib import Path
import sys
import time

VERSION = __VERSION__

if sys.argv[1:] == ["--version"]:
    print("ast-grep " + VERSION)
    raise SystemExit(0)

args = sys.argv[1:]
if not args or args[0] != "run" or "--update-all" in args:
    print("unexpected invocation", file=sys.stderr)
    raise SystemExit(97)
expected = ["--pattern", "--rewrite", "--lang", "--json=compact", "--"]
positions = []
for value in expected:
    if value not in args:
        print("missing " + value, file=sys.stderr)
        raise SystemExit(96)
    positions.append(args.index(value))
if positions != sorted(positions):
    print("argument order changed", file=sys.stderr)
    raise SystemExit(95)
pattern = args[args.index("--pattern") + 1]
rewrite = args[args.index("--rewrite") + 1]
language = args[args.index("--lang") + 1]
paths = args[args.index("--") + 1:]

if pattern == "CASE:malformed":
    sys.stdout.write("{")
    raise SystemExit(0)
if pattern == "CASE:exit":
    print("deliberate failure", file=sys.stderr)
    raise SystemExit(7)
if pattern == "CASE:large":
    sys.stdout.write("x" * 100000)
    raise SystemExit(0)
if pattern == "CASE:timeout":
    time.sleep(10)
    raise SystemExit(0)

rows = []
for name in paths:
    path = Path(name)
    source = path.read_bytes()
    before = b"old_api()"
    start = source.find(before)
    if start < 0:
        continue
    end = start + len(before)
    prefix = source[:start].decode("utf-8")
    line = prefix.count("\n")
    column = len(prefix.rsplit("\n", 1)[-1])
    row = {
        "text": before.decode(),
        "range": {
            "byteOffset": {"start": start, "end": end},
            "start": {"line": line, "column": column},
            "end": {"line": line, "column": column + len(before)},
        },
        "file": name,
        "lines": source.decode().splitlines()[line],
        "charCount": {"leading": column, "trailing": 0},
        "replacement": "new_api()",
        "replacementOffsets": {"start": start, "end": end},
        "language": "Python",
        "metaVariables": {"single": {}, "multi": {}},
        "transformed": {},
    }
    rows.append(row)

if pattern == "CASE:missing":
    del rows[0]["replacement"]
elif pattern == "CASE:extra":
    rows[0]["unexpected"] = True
elif pattern == "CASE:wrong-path":
    rows[0]["file"] = "../outside.py"
elif pattern == "CASE:wrong-text":
    rows[0]["text"] = "not source text"
elif pattern == "CASE:wrong-offset":
    rows[0]["range"]["byteOffset"]["start"] += 1
elif pattern == "CASE:bad-language":
    rows[0]["language"] = "JavaScript"
elif pattern == "CASE:duplicate":
    rows.append(dict(rows[0]))
elif pattern == "CASE:overlap":
    second = dict(rows[0])
    second["range"] = {
        "byteOffset": {
            "start": rows[0]["range"]["byteOffset"]["start"] + 1,
            "end": rows[0]["range"]["byteOffset"]["end"],
        },
        "start": {"line": 0, "column": 1},
        "end": {"line": 0, "column": 9},
    }
    second["replacementOffsets"] = dict(second["range"]["byteOffset"])
    source = Path(second["file"]).read_bytes()
    second["text"] = source[
        second["range"]["byteOffset"]["start"]:
        second["range"]["byteOffset"]["end"]
    ].decode()
    rows.append(second)
elif pattern == "CASE:mutate":
    Path(paths[0]).write_text("mutated\n", encoding="utf-8")

print(json.dumps(rows, ensure_ascii=False, separators=(",", ":")))
'''


class AstGrepAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        (self.root / "b.py").write_text(
            'label = "λ"\nresult = old_api()\n',
            encoding="utf-8",
        )
        (self.root / "a.py").write_text(
            "first = old_api()\n",
            encoding="utf-8",
        )
        self.executable = self._fake("0.40.4")
        self.provenance = inspect_ast_grep_executable(self.executable)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _fake(self, version: str, name: str = "ast-grep-fake") -> Path:
        target = self.root / name
        source = "#!" + sys.executable + "\n" + FAKE.replace(
            "__VERSION__",
            repr(version),
        )
        target.write_text(source, encoding="utf-8")
        target.chmod(0o755)
        return target

    def _materialize(
        self,
        pattern: str = "old_api($$$ARGS)",
        **overrides: object,
    ) -> tuple[dict[str, object], ...]:
        arguments: dict[str, object] = {
            "executable": self.executable,
            "expected_executable_sha256": self.provenance.sha256,
            "required_version": "0.40.4",
            "pattern": pattern,
            "rewrite": "new_api($$$ARGS)",
            "language": "python",
            "paths": ["b.py", "a.py"],
            "timeout_seconds": 2.0,
            "max_source_bytes": 4096,
            "max_output_bytes": 65536,
            "max_matches": 10,
        }
        arguments.update(overrides)
        return materialize_ast_grep_operations(self.root, **arguments)

    def test_materializes_sorted_source_locked_utf8_operations(self) -> None:
        first = self._materialize()
        second = self._materialize()
        self.assertEqual(first, second)
        self.assertEqual([row["path"] for row in first], ["a.py", "b.py"])
        self.assertEqual(first[0]["action"], "replace_range")
        self.assertEqual(first[0]["before"], "old_api()")
        self.assertEqual(first[0]["replacement"], "new_api()")
        self.assertEqual(
            first[0]["source_sha256"],
            hashlib.sha256((self.root / "a.py").read_bytes()).hexdigest(),
        )
        unicode_operation = first[1]
        source = (self.root / "b.py").read_bytes()
        start = int(unicode_operation["start_byte"])
        end = int(unicode_operation["end_byte"])
        self.assertEqual(source[start:end].decode(), "old_api()")
        character_offset = (self.root / "b.py").read_text().index("old_api()")
        self.assertEqual(start, character_offset + 1)
        self.assertIn("old_api", (self.root / "a.py").read_text())

    def test_requires_exact_executable_provenance_and_version(self) -> None:
        with self.assertRaisesRegex(AstGrepAdapterError, "SHA-256"):
            self._materialize(expected_executable_sha256="0" * 64)
        wrong = self._fake("0.41.0", "wrong-version")
        wrong_sha = hashlib.sha256(wrong.read_bytes()).hexdigest()
        with self.assertRaisesRegex(AstGrepAdapterError, "does not match"):
            self._materialize(
                executable=wrong,
                expected_executable_sha256=wrong_sha,
            )
        with self.assertRaisesRegex(AstGrepAdapterError, "absolute path"):
            self._materialize(executable=Path("ast-grep"))

    def test_rejects_unsafe_or_unbounded_inputs(self) -> None:
        with self.assertRaisesRegex(AstGrepAdapterError, "repository-relative"):
            self._materialize(paths=["../a.py"])
        with self.assertRaisesRegex(AstGrepAdapterError, "unique"):
            self._materialize(paths=["a.py", "a.py"])
        (self.root / "linked.py").symlink_to(self.root / "a.py")
        with self.assertRaisesRegex(AstGrepAdapterError, "symbolic link"):
            self._materialize(paths=["linked.py"])
        with self.assertRaisesRegex(AstGrepAdapterError, "inputs exceed"):
            self._materialize(max_source_bytes=1)
        with self.assertRaisesRegex(AstGrepAdapterError, "1..4096"):
            self._materialize(paths=[])

    def test_rejects_malformed_or_nonconforming_json(self) -> None:
        cases = {
            "CASE:malformed": "valid UTF-8 JSON",
            "CASE:missing": "keys differ",
            "CASE:extra": "keys differ",
            "CASE:wrong-path": "repository-relative",
            "CASE:wrong-text": "does not match",
            "CASE:wrong-offset": "does not match",
            "CASE:bad-language": "does not match requested",
        }
        for pattern, message in cases.items():
            with self.subTest(pattern=pattern):
                with self.assertRaisesRegex(AstGrepAdapterError, message):
                    self._materialize(pattern)

    def test_rejects_duplicate_and_overlapping_matches(self) -> None:
        with self.assertRaisesRegex(AstGrepAdapterError, "duplicate"):
            self._materialize("CASE:duplicate")
        with self.assertRaisesRegex(AstGrepAdapterError, "overlapping"):
            self._materialize("CASE:overlap")

    def test_rejects_process_failure_timeout_and_excess_output(self) -> None:
        with self.assertRaisesRegex(AstGrepAdapterError, "exited with 7"):
            self._materialize("CASE:exit")
        with self.assertRaisesRegex(AstGrepAdapterError, "deadline"):
            self._materialize("CASE:timeout", timeout_seconds=0.05)
        with self.assertRaisesRegex(AstGrepAdapterError, "output exceeds"):
            self._materialize("CASE:large", max_output_bytes=256)

    def test_detects_attempted_preview_mutation(self) -> None:
        before = (self.root / "a.py").read_bytes()
        with self.assertRaisesRegex(AstGrepAdapterError, "attempted to mutate"):
            self._materialize("CASE:mutate")
        self.assertEqual((self.root / "a.py").read_bytes(), before)

    def test_rejects_invalid_utf8_source(self) -> None:
        (self.root / "invalid.py").write_bytes(b"\xffold_api()")
        with self.assertRaisesRegex(AstGrepAdapterError, "not valid UTF-8"):
            self._materialize(paths=["invalid.py"])

    def test_optional_real_ast_grep_integration(self) -> None:
        executable = os.environ.get("ARCHBIRD_TEST_AST_GREP") or shutil.which(
            "ast-grep"
        )
        if executable is None:
            self.skipTest("real ast-grep executable is not installed")
        executable_path = Path(executable).resolve()
        completed = subprocess.run(
            [str(executable_path), "--version"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        version = completed.stdout.strip().split()[-1]
        provenance = inspect_ast_grep_executable(executable_path)
        operations = materialize_ast_grep_operations(
            self.root,
            executable=executable_path,
            expected_executable_sha256=provenance.sha256,
            required_version=version,
            pattern="old_api($$$ARGS)",
            rewrite="new_api($$$ARGS)",
            language="python",
            paths=["a.py", "b.py"],
        )
        self.assertEqual(len(operations), 2)
        self.assertIn("old_api", (self.root / "a.py").read_text())


if __name__ == "__main__":
    unittest.main()
