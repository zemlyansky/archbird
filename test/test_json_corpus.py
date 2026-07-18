#!/usr/bin/env python3
"""Gate strict parsing against the pinned JSONTestSuite corpus."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys


EXPECTED_COMMIT = "1ef36fa01286573e846ac449e8683f8833c5b26a"
EXPECTED_IMPLEMENTATION_DEFINED_DIGEST = (
    "4b4f0a7e27e056c818be992b2819d5a3ff720c7095a05404671e457255a9be48"
)
ARCHBIRD_STRICT_DIVERGENCES = {
    "y_object_duplicated_key.json",
    "y_object_duplicated_key_and_value.json",
}


def accepted(executable: Path, path: Path) -> tuple[bool, bytes]:
    completed = subprocess.run(
        [str(executable), "--validate"],
        input=path.read_bytes(),
        capture_output=True,
        check=False,
    )
    return completed.returncode == 0, completed.stderr


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_json_corpus.py JSON_CLI")
    executable = Path(sys.argv[1]).resolve()
    suite = Path(os.environ["JSON_TEST_SUITE_ROOT"]).resolve()
    corpus = suite / "test_parsing"
    commit = subprocess.run(
        ["git", "-C", str(suite), "rev-parse", "HEAD"],
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    if commit != EXPECTED_COMMIT:
        raise AssertionError(f"JSONTestSuite commit {commit}, expected {EXPECTED_COMMIT}")

    errors: list[str] = []
    classifications: list[str] = []
    counts = {"y_accept": 0, "y_strict_reject": 0, "n_reject": 0, "i_accept": 0, "i_reject": 0}
    for path in sorted(corpus.glob("*.json"), key=lambda item: item.name):
        result, stderr = accepted(executable, path)
        prefix = path.name[:2]
        if prefix == "y_":
            if path.name in ARCHBIRD_STRICT_DIVERGENCES:
                if result or b"error 4" not in stderr:
                    errors.append(f"{path.name}: expected duplicate-key rejection")
                else:
                    counts["y_strict_reject"] += 1
            elif not result:
                errors.append(f"{path.name}: valid JSON rejected: {stderr.decode()}")
            else:
                counts["y_accept"] += 1
        elif prefix == "n_":
            if result:
                errors.append(f"{path.name}: invalid JSON accepted")
            else:
                counts["n_reject"] += 1
        elif prefix == "i_":
            disposition = "accept" if result else "reject"
            classifications.append(f"{path.name}:{disposition}")
            counts[f"i_{disposition}"] += 1

    if errors:
        raise AssertionError("JSON corpus failures:\n" + "\n".join(errors))
    classification_digest = hashlib.sha256(
        ("\n".join(classifications) + "\n").encode()
    ).hexdigest()
    if classification_digest != EXPECTED_IMPLEMENTATION_DEFINED_DIGEST:
        raise AssertionError(
            "implementation-defined JSON classification changed: "
            f"{classification_digest} != {EXPECTED_IMPLEMENTATION_DEFINED_DIGEST}"
        )
    print(
        "JSONTestSuite passed: "
        + " ".join(f"{name}={value}" for name, value in counts.items())
        + f" i_digest={classification_digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
