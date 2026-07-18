#!/usr/bin/env python3
"""Differential probe against an optional maintainer reference."""

from __future__ import annotations

import json
import os
from pathlib import Path
import random
import subprocess
import sys

import archbird


def render_python(source: bytes, *, pretty: bool) -> bytes:
    value = json.loads(source)
    options: dict[str, object] = {
        "sort_keys": True,
        "ensure_ascii": False,
        "allow_nan": False,
    }
    if pretty:
        options["indent"] = 2
    else:
        options["separators"] = (",", ":")
    rendered = json.dumps(value, **options)
    if pretty:
        rendered += "\n"
    return rendered.encode("utf-8")


def render_native(executable: Path, source: bytes, *, pretty: bool) -> bytes:
    command = [str(executable)]
    if pretty:
        command.extend(["--pretty", "--newline"])
    completed = subprocess.run(command, input=source, capture_output=True, check=False)
    if completed.returncode != 0:
        raise AssertionError(
            f"native renderer failed for {source!r}: {completed.stderr.decode()}"
        )
    return completed.stdout


def generated_value(rng: random.Random, depth: int) -> object:
    alphabet = ["a", "z", "é", "𝄞", "\x00", "\n", "\\", '"']
    scalar_kind = rng.randrange(7 if depth else 5)
    if scalar_kind == 0:
        return None
    if scalar_kind == 1:
        return bool(rng.randrange(2))
    if scalar_kind == 2:
        sign = -1 if rng.randrange(2) else 1
        return sign * rng.randrange(10 ** rng.randrange(1, 80))
    if scalar_kind == 3:
        return "".join(rng.choice(alphabet) for _ in range(rng.randrange(10)))
    if scalar_kind == 4:
        return 0
    if scalar_kind == 5:
        return [generated_value(rng, depth - 1) for _ in range(rng.randrange(5))]
    result: dict[str, object] = {}
    target = rng.randrange(5)
    while len(result) < target:
        key = "".join(rng.choice(alphabet) for _ in range(rng.randrange(1, 8)))
        result[key] = generated_value(rng, depth - 1)
    return result


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_python_reference.py JSON_CLI")
    executable = Path(sys.argv[1]).resolve()
    reference = Path(os.environ["ARCHBIRD_REFERENCE_ROOT"]).resolve()
    loaded = Path(archbird.__file__).resolve()
    if reference not in loaded.parents:
        raise AssertionError(f"loaded {loaded}, expected reference under {reference}")

    case_bundle_path = Path(__file__).with_name("json_cases.json")
    case_bundle = json.loads(case_bundle_path.read_text(encoding="utf-8"))
    if case_bundle.get("schema_version") != 1:
        raise AssertionError("unsupported native JSON case-bundle schema")
    fixtures = [
        (case["id"], case["json"].encode("utf-8"))
        for case in case_bundle["cases"]
    ]
    generated = case_bundle["generated"]
    rng = random.Random(generated["seed"])
    for index in range(generated["count"]):
        value = generated_value(rng, generated["max_depth"])
        source = json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        ).encode("utf-8")
        if index % 2:
            source = b" \n" + source + b"\t "
        fixtures.append((f"generated-{index:03d}", source))
    for repeat in range(2):
        for pretty in (False, True):
            for case_id, source in fixtures:
                expected = render_python(source, pretty=pretty)
                actual = render_native(executable, source, pretty=pretty)
                if actual != expected:
                    raise AssertionError(
                        "JSON differential mismatch\n"
                        f"case={case_id} repeat={repeat} pretty={pretty} "
                        f"source={source!r}\n"
                        f"expected={expected!r}\nactual={actual!r}"
                    )

    real_source = b'{"real":1.0}'
    real_expected = render_python(real_source, pretty=False)
    real_actual = render_native(executable, real_source, pretty=False)
    if real_actual != real_expected:
        raise AssertionError(
            "native finite-real parity regressed\n"
            f"expected={real_expected!r}\nactual={real_actual!r}"
        )

    print(
        "Python/native JSON parity passed for ACJ-0; "
        f"cases={len(fixtures)} reference={archbird.__version__} "
        f"digest={archbird.implementation_digest()[:16]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
