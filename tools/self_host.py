#!/usr/bin/env python3
"""Content-addressed stable/candidate self-hosting for Archbird development.

This is a project-owned release harness, not part of libarchbird.  It executes
the supplied Archbird distributions and test command, while the core remains
I/O-free and read-only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Mapping, Sequence
import venv
import zipfile

try:
    from . import sync_csrc
except ImportError:  # Direct execution as ``python tools/self_host.py``.
    import sync_csrc


SCHEMA_VERSION = 1
SLOT_ARTIFACT = "archbird-self-host-slot"
STATE_ARTIFACT = "archbird-self-host-state"
GATE_ARTIFACT = "archbird-self-host-gate"


class SelfHostError(RuntimeError):
    pass


def positive_integer(value: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def canonical(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(value: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or "\\" in value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise SelfHostError(f"unsafe relative path {value!r}")
    return path.as_posix()


def read_json(path: Path, label: str) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise SelfHostError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise SelfHostError(f"{label} must be a JSON object")
    return value


def atomic_write(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(value)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def validate_wheel(path: Path) -> tuple[str, str]:
    if not path.is_file() or path.is_symlink() or path.suffix != ".whl":
        raise SelfHostError(f"candidate is not a regular wheel: {path}")
    version = ""
    try:
        with zipfile.ZipFile(path) as archive:
            metadata = []
            for name in archive.namelist():
                safe_relative(name.rstrip("/"))
                if name.endswith(".dist-info/METADATA"):
                    metadata.append(name)
            if len(metadata) != 1:
                raise SelfHostError("wheel must contain exactly one METADATA file")
            for line in archive.read(metadata[0]).decode("utf-8").splitlines():
                if line.startswith("Version: "):
                    version = line.removeprefix("Version: ").strip()
                    break
    except (OSError, UnicodeError, zipfile.BadZipFile) as error:
        raise SelfHostError(f"invalid wheel {path}: {error}") from error
    if not version:
        raise SelfHostError("wheel metadata has no version")
    return sha256_file(path), version


def executable(slot: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    candidate = slot / "venv" / ("Scripts" if os.name == "nt" else "bin") / (
        name + suffix
    )
    if not candidate.is_file() or candidate.is_symlink():
        raise SelfHostError(f"slot executable is missing: {candidate}")
    return candidate


def archbird_command(slot: Path) -> list[str]:
    return [str(executable(slot, "python")), "-m", "archbird"]


def archbird_console_command(slot: Path) -> list[str]:
    return [str(executable(slot, "archbird"))]


def slot_directory(state_root: Path, wheel_sha256: str) -> Path:
    return state_root / "slots" / wheel_sha256


def validate_slot(state_root: Path, wheel_sha256: str) -> Mapping[str, Any]:
    slot = slot_directory(state_root, wheel_sha256)
    document = read_json(slot / "slot.json", "self-host slot")
    expected = {
        "artifact": SLOT_ARTIFACT,
        "schema_version": SCHEMA_VERSION,
        "wheel_sha256": wheel_sha256,
    }
    for key, value in expected.items():
        if document.get(key) != value:
            raise SelfHostError(f"slot {wheel_sha256} has invalid {key}")
    wheel_name = safe_relative(str(document.get("wheel", "")))
    wheel = slot / wheel_name
    if not wheel.is_file() or wheel.is_symlink() or sha256_file(wheel) != wheel_sha256:
        raise SelfHostError(f"slot wheel bytes do not match {wheel_sha256}")
    for label, command in (
        ("module", archbird_command(slot)),
        ("console", archbird_console_command(slot)),
    ):
        version = subprocess.run(
            [*command, "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if version != document.get("version"):
            raise SelfHostError(
                f"installed slot {label} version differs from slot manifest"
            )
    return document


def install_slot(state_root: Path, wheel_path: Path) -> Mapping[str, Any]:
    wheel_sha256, version = validate_wheel(wheel_path)
    final = slot_directory(state_root, wheel_sha256)
    if final.exists():
        return validate_slot(state_root, wheel_sha256)
    final.parent.mkdir(parents=True, exist_ok=True)
    # Python virtual environments and their generated console scripts are not
    # relocatable: entrypoint shebangs bind to the environment's creation path.
    # The content-addressed final slot name is already known from the wheel, so
    # create the environment there and publish slot.json only after all checks.
    final.mkdir()
    try:
        wheel = final / wheel_path.name
        shutil.copyfile(wheel_path, wheel)
        if sha256_file(wheel) != wheel_sha256:
            raise SelfHostError("wheel changed while creating self-host slot")
        venv.EnvBuilder(with_pip=True, clear=False, symlinks=False).create(
            final / "venv"
        )
        python = executable(final, "python")
        subprocess.run(
            [
                str(python),
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                "--no-deps",
                str(wheel),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        subprocess.run(
            [str(python), "-m", "pip", "check"],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        for label, command in (
            ("module", archbird_command(final)),
            ("console", archbird_console_command(final)),
        ):
            actual_version = subprocess.run(
                [*command, "--version"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            if actual_version != version:
                raise SelfHostError(
                    f"wheel metadata version {version!r} differs from "
                    f"{label} CLI {actual_version!r}"
                )
        slot = {
            "artifact": SLOT_ARTIFACT,
            "schema_version": SCHEMA_VERSION,
            "version": version,
            "wheel": wheel.name,
            "wheel_sha256": wheel_sha256,
        }
        atomic_write(final / "slot.json", canonical(slot) + b"\n")
    except BaseException:
        shutil.rmtree(final)
        raise
    return validate_slot(state_root, wheel_sha256)


def state_path(state_root: Path) -> Path:
    return state_root / "stable.json"


def load_state(state_root: Path) -> Mapping[str, Any]:
    state = read_json(state_path(state_root), "self-host state")
    if state.get("artifact") != STATE_ARTIFACT or state.get("schema_version") != 1:
        raise SelfHostError("invalid self-host state artifact")
    stable = state.get("stable")
    if not isinstance(stable, dict) or not isinstance(stable.get("wheel_sha256"), str):
        raise SelfHostError("self-host state has no stable slot")
    validate_slot(state_root, stable["wheel_sha256"])
    return state


def seed(args: argparse.Namespace) -> int:
    state_root = Path(args.state).resolve()
    if state_path(state_root).exists():
        raise SelfHostError("self-host state already exists")
    slot = install_slot(state_root, Path(args.wheel).resolve())
    state = {
        "artifact": STATE_ARTIFACT,
        "generation": 0,
        "previous": None,
        "schema_version": SCHEMA_VERSION,
        "stable": {
            "version": slot["version"],
            "wheel_sha256": slot["wheel_sha256"],
        },
    }
    atomic_write(state_path(state_root), canonical(state) + b"\n")
    print(slot["wheel_sha256"])
    return 0


def core_source_digest(root: Path) -> str:
    if root.resolve() != sync_csrc.REPOSITORY.resolve():
        raise SelfHostError(
            "self-host source identity must be computed from this checkout"
        )
    return sync_csrc.repository_implementation_digest()


def python_source_digest(root: Path) -> str:
    package = root / "py" / "archbird"
    digest = hashlib.sha256()
    for path in sorted(package.rglob("*.py"), key=lambda p: p.relative_to(package).as_posix()):
        relative = path.relative_to(package).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(bytes.fromhex(sha256_file(path)))
        digest.update(b"\0")
    return digest.hexdigest()


def package_identity(slot: Path) -> Mapping[str, str]:
    script = (
        "import archbird,json;"
        "print(json.dumps({'version':archbird.__version__,"
        "'python_implementation_sha256':archbird.implementation_digest()},"
        "sort_keys=True,separators=(',',':')))"
    )
    completed = subprocess.run(
        [str(executable(slot, "python")), "-c", script],
        check=True,
        capture_output=True,
        text=True,
    )
    value = json.loads(completed.stdout)
    if not isinstance(value, dict):
        raise SelfHostError("installed package identity is invalid")
    return value


def run_to_files(
    command: Sequence[str], cwd: Path, stdout_path: Path, stderr_path: Path
) -> Mapping[str, Any]:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        completed = subprocess.run(command, cwd=cwd, stdout=stdout, stderr=stderr)
    stderr_bytes = stderr_path.read_bytes()
    return {
        "argv": list(command),
        "exit_code": completed.returncode,
        "stderr_sha256": sha256_file(stderr_path),
        "stderr_tail": stderr_bytes[-4096:].decode("utf-8", errors="replace"),
        "stdout_sha256": sha256_file(stdout_path),
    }


def verify_supported(command: Sequence[str]) -> bool:
    completed = subprocess.run(
        [*command, "verify", "--help"], capture_output=True, text=True
    )
    return completed.returncode == 0 and completed.stdout.startswith(
        "usage: archbird verify"
    )


def normalized_map(document: Mapping[str, Any]) -> bytes:
    value = dict(document)
    value.pop("tool", None)
    return canonical(value)


def source_stability_map(document: Mapping[str, Any]) -> bytes:
    """Project source/structure evidence, excluding producer and discovery context."""

    value = dict(document)
    value.pop("discovery", None)
    value.pop("tool", None)
    return canonical(value)


def test_stability(
    before: Mapping[str, Any], after: Mapping[str, Any]
) -> Mapping[str, Any]:
    before_source = source_stability_map(before)
    after_source = source_stability_map(after)
    before_discovery = canonical(before.get("discovery"))
    after_discovery = canonical(after.get("discovery"))
    return {
        "discovery_context": {
            "after_sha256": sha256_bytes(after_discovery),
            "before_sha256": sha256_bytes(before_discovery),
            "equal": before_discovery == after_discovery,
        },
        "source_evidence": {
            "after_sha256": sha256_bytes(after_source),
            "before_sha256": sha256_bytes(before_source),
            "equal": before_source == after_source,
        },
    }


def sealed(document: Mapping[str, Any]) -> Mapping[str, Any]:
    value = dict(document)
    value["seal_sha256"] = sha256_bytes(canonical(value))
    return value


def validate_gate(path: Path) -> Mapping[str, Any]:
    gate = read_json(path, "self-host gate")
    if gate.get("artifact") != GATE_ARTIFACT or gate.get("schema_version") != 1:
        raise SelfHostError("invalid self-host gate artifact")
    seal = gate.get("seal_sha256")
    unsigned = dict(gate)
    unsigned.pop("seal_sha256", None)
    if not isinstance(seal, str) or seal != sha256_bytes(canonical(unsigned)):
        raise SelfHostError("self-host gate seal is invalid")
    return gate


def check(args: argparse.Namespace) -> int:
    state_root = Path(args.state).resolve()
    root = Path(args.root).resolve()
    config = Path(args.config).resolve()
    suite = Path(args.verify_suite).resolve()
    output = Path(args.output).resolve()
    state = load_state(state_root)
    candidate = install_slot(state_root, Path(args.wheel).resolve())
    stable_sha = state["stable"]["wheel_sha256"]
    candidate_sha = candidate["wheel_sha256"]
    stable_slot = slot_directory(state_root, stable_sha)
    candidate_slot = slot_directory(state_root, candidate_sha)
    evidence = output.parent / f"{output.stem}.evidence"
    if evidence.exists():
        raise SelfHostError(f"gate evidence already exists: {evidence}")
    evidence.mkdir(parents=True)
    commands: dict[str, Any] = {}

    def run_map(label: str, slot: Path) -> Mapping[str, Any]:
        destination = evidence / f"{label}.map.json"
        command = [
            *archbird_command(slot),
            "--config",
            str(config),
            "--root",
            str(root),
            "--jobs",
            "1",
            "--format",
            "json",
            "--check",
            "--output",
            str(destination),
        ]
        if args.max_file_bytes is not None:
            command.extend(["--max-file-bytes", str(args.max_file_bytes)])
        record = run_to_files(
            command, root, evidence / f"{label}.map.stdout", evidence / f"{label}.map.stderr"
        )
        commands[f"{label}_map"] = record
        if record["exit_code"] != 0:
            raise SelfHostError(f"{label} map failed: {record['stderr_tail']}")
        return read_json(destination, f"{label} map")

    stable_map = run_map("stable", stable_slot)
    candidate_map_before = run_map("candidate-before-tests", candidate_slot)
    test_stdout = evidence / "tests.stdout"
    test_stderr = evidence / "tests.stderr"
    test_record = run_to_files(args.test_command, root, test_stdout, test_stderr)
    commands["tests"] = test_record
    if test_record["exit_code"] != 0:
        raise SelfHostError(f"candidate tests failed: {test_record['stderr_tail']}")
    candidate_map = run_map("candidate", candidate_slot)
    stability = test_stability(candidate_map_before, candidate_map)
    if not stability["source_evidence"]["equal"]:
        raise SelfHostError("source evidence changed while candidate tests ran")

    verification: dict[str, Any] = {}
    for label, slot, required in (
        ("stable", stable_slot, False),
        ("candidate", candidate_slot, True),
    ):
        binary = archbird_command(slot)
        if not verify_supported(binary):
            if required:
                raise SelfHostError("candidate distribution has no Verify CLI")
            if int(state["generation"]) != 0:
                raise SelfHostError(
                    "only bootstrap generation zero may lack stable Verify"
                )
            verification[label] = {"status": "unsupported"}
            continue
        destination = evidence / f"{label}.verification.json"
        command = [
            *binary,
            "verify",
            "--config",
            str(suite),
            "--project-root",
            f"subject={root}",
            "--jobs",
            "1",
            "--format",
            "json",
            "--check",
            "--output",
            str(destination),
        ]
        record = run_to_files(
            command,
            root,
            evidence / f"{label}.verify.stdout",
            evidence / f"{label}.verify.stderr",
        )
        commands[f"{label}_verify"] = record
        if record["exit_code"] != 0:
            raise SelfHostError(
                f"{label} self-verification failed: {record['stderr_tail']}"
            )
        verification[label] = {
            "result_sha256": sha256_file(destination),
            "status": "pass",
        }

    source_core = core_source_digest(root)
    source_python = python_source_digest(root)
    candidate_identity = package_identity(candidate_slot)
    candidate_tool = candidate_map.get("tool")
    if not isinstance(candidate_tool, dict):
        raise SelfHostError("candidate map has no tool identity")
    bindings = {
        "core": candidate_tool.get("implementation_sha256") == source_core,
        "python": candidate_identity.get("python_implementation_sha256")
        == source_python,
    }
    if not all(bindings.values()):
        raise SelfHostError(
            "candidate distribution is not built from the checked source closure"
        )

    stable_normalized = normalized_map(stable_map)
    candidate_normalized = normalized_map(candidate_map)
    map_equal = stable_normalized == candidate_normalized
    delta_sha256 = sha256_bytes(
        canonical(
            {
                "candidate": sha256_bytes(candidate_normalized),
                "stable": sha256_bytes(stable_normalized),
            }
        )
    )
    gate = sealed(
        {
            "artifact": GATE_ARTIFACT,
            "bindings": bindings,
            "candidate": {
                "version": candidate["version"],
                "wheel_sha256": candidate_sha,
            },
            "commands": commands,
            "maps": {
                "candidate_sha256": sha256_bytes(candidate_normalized),
                "delta_sha256": delta_sha256,
                "equal": map_equal,
                "stable_sha256": sha256_bytes(stable_normalized),
            },
            "schema_version": SCHEMA_VERSION,
            "source": {
                "config_sha256": sha256_file(config),
                "core_implementation_sha256": source_core,
                "map_input_sha256": candidate_map.get("evidence", {}).get(
                    "input_sha256"
                ),
                "python_implementation_sha256": source_python,
                "suite_sha256": sha256_file(suite),
            },
            "stable": {
                "generation": state["generation"],
                "version": state["stable"]["version"],
                "wheel_sha256": stable_sha,
            },
            "status": "pass" if map_equal else "review-required",
            "test_stability": stability,
            "verification": verification,
        }
    )
    atomic_write(output, canonical(gate) + b"\n")
    print(gate["status"])
    print(gate["seal_sha256"])
    return 0 if map_equal else 3


def promote(args: argparse.Namespace) -> int:
    state_root = Path(args.state).resolve()
    state = load_state(state_root)
    gate = validate_gate(Path(args.gate).resolve())
    if gate["stable"]["wheel_sha256"] != state["stable"]["wheel_sha256"]:
        raise SelfHostError("stable slot changed after the gate was created")
    if gate["status"] == "review-required":
        if args.approve_map_delta != gate["maps"]["delta_sha256"]:
            raise SelfHostError(
                "promotion requires the exact reviewed --approve-map-delta digest"
            )
        if not args.rationale:
            raise SelfHostError("reviewed map delta requires --rationale")
    elif gate["status"] != "pass":
        raise SelfHostError(f"gate status {gate['status']!r} cannot be promoted")
    candidate = gate["candidate"]
    validate_slot(state_root, candidate["wheel_sha256"])
    if gate["verification"]["candidate"]["status"] != "pass" or not all(
        gate["bindings"].values()
    ):
        raise SelfHostError("candidate verification or source binding is incomplete")
    tests = gate["commands"].get("tests")
    if not isinstance(tests, dict) or tests.get("exit_code") != 0:
        raise SelfHostError("candidate test evidence is absent or failing")
    next_state = {
        "artifact": STATE_ARTIFACT,
        "generation": int(state["generation"]) + 1,
        "previous": state["stable"],
        "promotion": {
            "gate_seal_sha256": gate["seal_sha256"],
            "map_delta_rationale": args.rationale or "",
        },
        "schema_version": SCHEMA_VERSION,
        "stable": {
            "version": candidate["version"],
            "wheel_sha256": candidate["wheel_sha256"],
        },
    }
    atomic_write(state_path(state_root), canonical(next_state) + b"\n")
    print(candidate["wheel_sha256"])
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Gate Archbird self-hosting through stable/candidate slots."
    )
    commands = result.add_subparsers(dest="command", required=True)
    seed_parser = commands.add_parser("seed")
    seed_parser.add_argument("--state", required=True)
    seed_parser.add_argument("--wheel", required=True)
    seed_parser.set_defaults(handler=seed)
    check_parser = commands.add_parser("check")
    check_parser.add_argument("--state", required=True)
    check_parser.add_argument("--wheel", required=True)
    check_parser.add_argument("--root", required=True)
    check_parser.add_argument("--config", required=True)
    check_parser.add_argument("--verify-suite", required=True)
    check_parser.add_argument("--output", required=True)
    check_parser.add_argument(
        "--max-file-bytes",
        type=positive_integer,
        help="common Map file limit override for both stable and candidate CLIs",
    )
    check_parser.add_argument(
        "test_command",
        nargs=argparse.REMAINDER,
        help="test command after --; it is executed without a shell",
    )
    check_parser.set_defaults(handler=check)
    promote_parser = commands.add_parser("promote")
    promote_parser.add_argument("--state", required=True)
    promote_parser.add_argument("--gate", required=True)
    promote_parser.add_argument("--approve-map-delta")
    promote_parser.add_argument("--rationale")
    promote_parser.set_defaults(handler=promote)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if args.command == "check":
        if args.test_command and args.test_command[0] == "--":
            args.test_command = args.test_command[1:]
        if not args.test_command:
            raise SelfHostError("check requires a test command after --")
    return int(args.handler(args))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.SubprocessError, SelfHostError, ValueError) as error:
        print(f"archbird self-host: error: {error}", file=sys.stderr)
        raise SystemExit(2)
