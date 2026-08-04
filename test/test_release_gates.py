#!/usr/bin/env python3
"""Prove that release gates reject representative publish-time defects."""

from __future__ import annotations

import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile


REPOSITORY = Path(__file__).resolve().parents[1]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NATIVE_CONTRACT = _load(
    "archbird_check_python_native_contract",
    REPOSITORY / "test/check_python_native_contract.py",
)
ATTESTATION = _load(
    "archbird_create_release_attestation",
    REPOSITORY / "tools/create_release_attestation.py",
)
STREAMING = _load(
    "archbird_release_streaming_contract",
    REPOSITORY / "test/release_streaming_contract.py",
)
README_CONTRACT = _load(
    "archbird_readme_contract",
    REPOSITORY / "test/test_readme_examples.py",
)


def _run(
    *arguments: str,
    cwd: Path,
    success: bool = True,
    contains: str | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if (result.returncode == 0) != success:
        raise AssertionError(
            f"unexpected status {result.returncode}: {' '.join(arguments)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    combined = result.stdout + result.stderr
    if contains is not None and contains not in combined:
        raise AssertionError(
            f"missing {contains!r}: {' '.join(arguments)}\n{combined}"
        )
    return result


def _git(repository: Path, *arguments: str) -> str:
    return _run("git", *arguments, cwd=repository).stdout.strip()


def _tar(path: Path, members: dict[str, bytes]) -> None:
    with tarfile.open(path, "w:gz") as archive:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))


def _zip(path: Path, members: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in members.items():
            archive.writestr(name, data)


def _write_json(path: Path, document: object) -> None:
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def test_native_contract_mutations() -> None:
    healthy = {
        "wrapper_references": {"IMPLEMENTATION_SHA256", "project_map"},
        "wrapper_calls": {"project_map"},
        "ctypes_operations": {"project_map", "project_write_map"},
        "compiled_operations": {"project_map", "project_write_map"},
        "compiled_attributes": {
            "IMPLEMENTATION_SHA256",
            "project_map",
            "project_write_map",
        },
    }
    if NATIVE_CONTRACT.contract_failures(**healthy):
        raise AssertionError("healthy native contract was rejected")

    missing_stream = dict(healthy)
    missing_stream["compiled_operations"] = {"project_map"}
    missing_stream["compiled_attributes"] = {
        "IMPLEMENTATION_SHA256",
        "project_map",
    }
    failures = NATIVE_CONTRACT.contract_failures(**missing_stream)
    if failures.get("ctypes operations absent from compiled extension") != [
        "project_write_map"
    ]:
        raise AssertionError("missing compiled streaming operation was not detected")

    missing_call = dict(healthy)
    missing_call["wrapper_calls"] = {"project_map", "project_write_map"}
    missing_call["ctypes_operations"] = {"project_map"}
    failures = NATIVE_CONTRACT.contract_failures(**missing_call)
    if failures.get("wrapper calls absent from ctypes adapter") != [
        "project_write_map"
    ]:
        raise AssertionError("missing ctypes wrapper operation was not detected")


def test_streaming_contract_mutations() -> None:
    class Project:
        def __init__(
            self,
            *,
            ignore_short: bool = False,
            replace_sink_error: bool = False,
        ) -> None:
            self.ignore_short = ignore_short
            self.replace_sink_error = replace_sink_error

        def map_json(self) -> bytes:
            return b'{"artifact":"map"}'

        def write_map_json(self, sink) -> None:
            payload = self.map_json()
            try:
                written = sink(payload)
            except Exception as error:
                if self.replace_sink_error:
                    raise RuntimeError("replacement") from error
                raise
            if (
                isinstance(written, int)
                and written != len(payload)
                and not self.ignore_short
            ):
                raise OSError("short write")

    STREAMING.validate_streaming_contract(Project)
    try:
        STREAMING.validate_streaming_contract(
            lambda: Project(ignore_short=True)
        )
    except AssertionError as error:
        if "accepted a short write" not in str(error):
            raise
    else:
        raise AssertionError("short-write mutation escaped the release gate")
    try:
        STREAMING.validate_streaming_contract(
            lambda: Project(replace_sink_error=True)
        )
    except RuntimeError as error:
        if str(error) != "replacement":
            raise
    else:
        raise AssertionError("sink-exception mutation escaped the release gate")


def test_documentation_contract_mutation(root: Path) -> None:
    path = root / "README.md"
    text = (
        "<!-- archbird-example: illustrative mutation-path -->\n"
        "```bash\n"
        "archbird path src/a.c src/b.c --check\n"
        "```\n"
    )
    try:
        README_CONTRACT.classified_shell_blocks(path, text)
    except AssertionError as error:
        if "every --check example" not in str(error):
            raise
    else:
        raise AssertionError(
            "candidate/illustrative checked Path mutation escaped the "
            "documentation gate"
        )


def _initialize_repository(root: Path) -> tuple[str, str, bytes, bytes, bytes]:
    (root / "tools").mkdir(parents=True)
    (root / "py/archbird").mkdir(parents=True)
    (root / "js").mkdir()
    shutil.copyfile(
        REPOSITORY / "tools/stage_release_provenance.py",
        root / "tools/stage_release_provenance.py",
    )
    python_readme = b"# Archbird Python\n"
    node_readme = b"# Archbird Node\n"
    (root / "py/README.md").write_bytes(python_readme)
    (root / "js/README.md").write_bytes(node_readme)
    (root / "py/pyproject.toml").write_text(
        '[project]\nname = "archbird"\nversion = "0.0.3"\n',
        encoding="utf-8",
    )
    package = {
        "name": "archbird",
        "version": "0.0.3",
        "files": ["README.md", "release.json"],
    }
    package_bytes = (
        json.dumps(package, ensure_ascii=False, indent=2) + "\n"
    ).encode("utf-8")
    (root / "js/package.json").write_bytes(package_bytes)
    _git(root, "init", "-q")
    _git(root, "config", "user.name", "Archbird Release Gate")
    _git(root, "config", "user.email", "release-gate@example.invalid")
    _git(root, "add", ".")
    _git(root, "commit", "-qm", "fixture")
    _git(root, "tag", "v0.0.3")
    commit = _git(root, "rev-parse", "HEAD")
    tree = _git(root, "rev-parse", "HEAD^{tree}")
    return commit, tree, python_readme, node_readme, package_bytes


def _release_archives(
    root: Path,
    *,
    commit: str,
    tree: str,
    python_readme: bytes,
    node_readme: bytes,
    package_bytes: bytes,
) -> tuple[Path, Path, Path]:
    release = {
        "artifact": "archbird-release-source",
        "schema_version": 1,
        "source_commit": commit,
        "source_tree": tree,
        "tag": "v0.0.3",
        "version": "0.0.3",
    }
    encoded = (
        json.dumps(release, ensure_ascii=True, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    c_source_manifest = b'{"artifact":"archbird-c-source-snapshot"}\n'
    schema_manifest = b'{"artifact":"archbird-schema-snapshot"}\n'
    wheel = root / "archbird-0.0.3-cp310-cp310-linux_x86_64.whl"
    sdist = root / "archbird-0.0.3.tar.gz"
    npm = root / "archbird-0.0.3.tgz"
    _zip(
        wheel,
        {
            "archbird/release.json": encoded,
            "archbird/schemas/.archbird-manifest.json": schema_manifest,
            "archbird-0.0.3.dist-info/METADATA": (
                b"Metadata-Version: 2.4\nName: archbird\nVersion: 0.0.3\n\n"
                + python_readme
            ),
        },
    )
    _tar(
        sdist,
        {
            "archbird-0.0.3/README.md": python_readme,
            "archbird-0.0.3/archbird/release.json": encoded,
            "archbird-0.0.3/archbird/schemas/.archbird-manifest.json": (
                schema_manifest
            ),
            "archbird-0.0.3/csrc/.archbird-manifest.json": c_source_manifest,
        },
    )
    package = json.loads(package_bytes)
    package["gitHead"] = commit
    _tar(
        npm,
        {
            "package/README.md": node_readme,
            "package/csrc/.archbird-manifest.json": c_source_manifest,
            "package/package.json": (
                json.dumps(package, ensure_ascii=False, indent=2) + "\n"
            ).encode("utf-8"),
            "package/release.json": encoded,
            "package/schema/.archbird-manifest.json": schema_manifest,
        },
    )
    return wheel, sdist, npm


def _conformance_reports(root: Path, provenance: Path) -> dict[str, Path]:
    version = "0.0.3"
    provenance_document = json.loads(provenance.read_text(encoding="utf-8"))
    transition = root / "release-transition.json"
    _write_json(
        transition,
        {
            "after_freshness": {"sha256": "a" * 64, "status": "current"},
            "after_map": {"input_sha256": "1" * 64, "sha256": "2" * 64},
            "artifact": "archbird-release-transition",
            "before_commit": "3" * 40,
            "before_map": {"input_sha256": "4" * 64, "sha256": "5" * 64},
            "before_ref": "v0.0.2",
            "before_freshness": {"sha256": "b" * 64, "status": "current"},
            "diff_sha256": "6" * 64,
            "expected_changes_sha256": "c" * 64,
            "failures": [],
            "source_commit": provenance_document["source_commit"],
            "source_tree": provenance_document["source_tree"],
            "tag": provenance_document["tag"],
            "verification_result_sha256": "7" * 64,
            "verification_sha256": "8" * 64,
            "version": version,
        },
    )
    reports: dict[str, Path] = {}
    for label, artifact in ATTESTATION.EXPECTED_REPORTS.items():
        document: dict[str, object] = {
            "artifact": artifact,
            "implementation_sha256": "9" * 64,
            "version": version,
        }
        if label.endswith("-contract"):
            document.update(
                {
                    "compiled_operations": ["project_map"],
                    "ctypes_operations": ["project_map"],
                    "extra_compiled_operations": [],
                    "missing_compiled_calls": [],
                    "missing_compiled_operations": [],
                    "missing_compiled_references": [],
                    "missing_ctypes_calls": [],
                    "wrapper_calls": ["project_map"],
                }
            )
        else:
            document["operations"] = sorted(ATTESTATION.EXPECTED_OPERATIONS[label])
            if label in {
                "python-sdist-runtime",
                "python-wheel-runtime",
                "node-native-prebuilt",
                "node-native-rebuilt",
            }:
                document["map_sha256"] = "8" * 64
            if label in {"node-cli-native", "node-cli-wasm"}:
                document["parity"] = {
                    name: "7" * 64 for name in sorted(ATTESTATION.PARITY_KEYS)
                }
            if label == "node-readme":
                document["documents"] = ["README.md", "js/README.md"]
                document["examples"] = [
                    {
                        "category": "tested-pass",
                        "covers": ["map"],
                        "id": "node-map",
                        "line": 1,
                    }
                ]
                document["surfaces"] = {
                    "browser_api": ["createBrowserArchbird"],
                    "cli": ["map"],
                    "entrypoints": ["archbird"],
                    "node_api": ["Project"],
                }
            elif label in {"python-sdist-readme", "python-wheel-readme"}:
                document["documents"] = ["README.md", "py/README.md"]
                document["examples"] = [
                    {
                        "category": "tested-pass",
                        "covers": ["map"],
                        "id": "python-map",
                        "line": 1,
                    }
                ]
                document["surfaces"] = {
                    "c_api": ["archbird_project_create"],
                    "cli": ["map"],
                    "python_api": ["Project"],
                }
            if label in ATTESTATION.EXPECTED_ENGINES:
                document["engine"] = ATTESTATION.EXPECTED_ENGINES[label]
        path = root / f"{label}.json"
        _write_json(path, document)
        reports[label] = path

    command = [
        sys.executable,
        str(REPOSITORY / "tools/create_release_attestation.py"),
        "--provenance",
        str(provenance),
        "--transition",
        str(transition),
    ]
    for label, path in reports.items():
        command.extend(("--report", f"{label}={path}"))
    command.extend(("--output", str(root / "release-attestation.json")))
    _run(*command, cwd=root)

    mutated = json.loads(reports["python-wheel-runtime"].read_text(encoding="utf-8"))
    for operation in (
        "map-streamed",
        "map-streamed-short-write",
        "map-streamed-sink-exception",
    ):
        mutated["operations"].remove(operation)
        _write_json(reports["python-wheel-runtime"], mutated)
        _run(
            *command,
            cwd=root,
            success=False,
            contains="conformance operation inventory differs",
        )
        mutated["operations"].append(operation)
    mutated["version"] = "0.0.2"
    _write_json(reports["python-wheel-runtime"], mutated)
    _run(*command, cwd=root, success=False, contains="report version differs")
    mutated["version"] = version
    _write_json(reports["python-wheel-runtime"], mutated)

    sdist_runtime = json.loads(
        reports["python-sdist-runtime"].read_text(encoding="utf-8")
    )
    sdist_runtime["map_sha256"] = "0" * 64
    _write_json(reports["python-sdist-runtime"], sdist_runtime)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="wheel and sdist-rebuilt wheel Maps differ",
    )
    sdist_runtime["map_sha256"] = "8" * 64
    _write_json(reports["python-sdist-runtime"], sdist_runtime)

    wasm_cli = json.loads(reports["node-cli-wasm"].read_text(encoding="utf-8"))
    wasm_cli["parity"]["map"] = "0" * 64
    _write_json(reports["node-cli-wasm"], wasm_cli)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="Node native and Wasm canonical results differ",
    )
    wasm_cli["parity"]["map"] = "7" * 64
    _write_json(reports["node-cli-wasm"], wasm_cli)

    browser = json.loads(reports["browser-wasm"].read_text(encoding="utf-8"))
    browser["implementation_sha256"] = "0" * 64
    _write_json(reports["browser-wasm"], browser)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="different core implementation identities",
    )
    browser["implementation_sha256"] = "9" * 64
    _write_json(reports["browser-wasm"], browser)

    wheel_readme = json.loads(
        reports["python-wheel-readme"].read_text(encoding="utf-8")
    )
    wheel_readme["examples"][0]["covers"] = ["not-observed"]
    _write_json(reports["python-wheel-readme"], wheel_readme)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="executable example coverage is invalid",
    )
    wheel_readme["examples"][0]["covers"] = ["map"]
    _write_json(reports["python-wheel-readme"], wheel_readme)

    transition_document = json.loads(transition.read_text(encoding="utf-8"))
    transition_document["source_commit"] = "0" * 40
    _write_json(transition, transition_document)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="self-transition did not pass",
    )
    return reports


def _test_transition_checker(repository: Path, root: Path) -> None:
    tool = {
        "implementation_sha256": "9" * 64,
        "name": "archbird",
        "version": "0.0.4",
    }
    before_map = {
        "artifact": "map",
        "diagnostics": [],
        "evidence": {"input_sha256": "1" * 64},
        "project": "release-gate",
        "tool": tool,
    }
    after_map = {
        "artifact": "map",
        "diagnostics": [],
        "evidence": {"input_sha256": "2" * 64},
        "project": "release-gate",
        "tool": tool,
    }
    sections: dict[str, dict[str, list[str]]] = {}
    for name in (
        "entrypoints",
        "package_dependencies",
        "package_entrypoint_surfaces",
        "package_export_origins",
        "package_exports",
        "parity_gaps",
        "public_symbols",
    ):
        sections[name] = {"added": [], "changed": [], "removed": []}
    sections["files"] = {
        "added": ["after-tag.txt"],
        "changed": [],
        "removed": [],
    }
    diff = {
        "after": {
            "input_sha256": "2" * 64,
            "project": "release-gate",
        },
        "artifact": "diff",
        "before": {
            "input_sha256": "1" * 64,
            "project": "release-gate",
        },
        "sections": sections,
        "tool": tool,
    }
    verification = {
        "artifact": "verification",
        "summary": {
            "blocking": False,
            "constraints": {"fail": 0, "unknown": 0},
        },
        "tool": tool,
        "verification_result_sha256": "3" * 64,
    }
    before_freshness = {
        "artifact": "map-freshness",
        "current": {"artifact": "map", "input_sha256": "1" * 64},
        "snapshot": {"artifact": "map", "input_sha256": "1" * 64},
        "status": "current",
    }
    after_freshness = {
        "artifact": "map-freshness",
        "current": {"artifact": "map", "input_sha256": "2" * 64},
        "snapshot": {"artifact": "map", "input_sha256": "2" * 64},
        "status": "current",
    }
    expected_changes = {
        "artifact": "archbird-release-change-envelope",
        "before_ref": "v0.0.3",
        "paths": ["after-tag.txt"],
        "version": "0.0.4",
    }
    paths = {
        "before": root / "transition-before.json",
        "after": root / "transition-after.json",
        "before_freshness": root / "transition-before-freshness.json",
        "after_freshness": root / "transition-after-freshness.json",
        "diff": root / "transition-diff.json",
        "expected": root / "transition-expected.json",
        "verification": root / "transition-verification.json",
        "output": root / "transition-report.json",
    }
    _write_json(paths["before"], before_map)
    _write_json(paths["after"], after_map)
    _write_json(paths["diff"], diff)
    _write_json(paths["before_freshness"], before_freshness)
    _write_json(paths["after_freshness"], after_freshness)
    _write_json(paths["expected"], expected_changes)
    _write_json(paths["verification"], verification)
    command = (
        sys.executable,
        str(REPOSITORY / "test/check_release_transition.py"),
        "--repository",
        str(repository),
        "--before-ref",
        "v0.0.3",
        "--version",
        "0.0.4",
        "--before-map",
        str(paths["before"]),
        "--after-map",
        str(paths["after"]),
        "--before-freshness",
        str(paths["before_freshness"]),
        "--after-freshness",
        str(paths["after_freshness"]),
        "--diff",
        str(paths["diff"]),
        "--expected-changes",
        str(paths["expected"]),
        "--verification",
        str(paths["verification"]),
        "--output",
        str(paths["output"]),
    )
    _run(*command, cwd=root)
    expected_changes["paths"].append("not-in-git.c")
    _write_json(paths["expected"], expected_changes)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="differs from reviewed change envelope",
    )
    expected_changes["paths"].remove("not-in-git.c")
    _write_json(paths["expected"], expected_changes)
    after_freshness["status"] = "stale"
    _write_json(paths["after_freshness"], after_freshness)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="candidate Map freshness",
    )
    after_freshness["status"] = "current"
    _write_json(paths["after_freshness"], after_freshness)
    diff["sections"]["files"]["added"].append("not-in-git.c")
    _write_json(paths["diff"], diff)
    _run(
        *command,
        cwd=root,
        success=False,
        contains="paths absent from Git transition",
    )


def test_release_provenance_and_attestation(root: Path) -> None:
    repository = root / "release-repository"
    repository.mkdir()
    commit, tree, python_readme, node_readme, package_bytes = (
        _initialize_repository(repository)
    )
    wheel, sdist, npm = _release_archives(
        root,
        commit=commit,
        tree=tree,
        python_readme=python_readme,
        node_readme=node_readme,
        package_bytes=package_bytes,
    )
    provenance = root / "release-provenance.json"
    checker = REPOSITORY / "test/check_release_provenance.py"
    check_command = (
        sys.executable,
        str(checker),
        "--repository",
        str(repository),
        "--version",
        "0.0.3",
        "--require-clean",
        "--require-tag",
        "--output",
        str(provenance),
        str(wheel),
        str(sdist),
        str(npm),
    )
    _run(*check_command, cwd=root)
    _conformance_reports(root, provenance)

    forged = root / "forged-archbird-0.0.3.whl"
    with zipfile.ZipFile(wheel) as source, zipfile.ZipFile(forged, "w") as target:
        for info in source.infolist():
            data = source.read(info)
            if info.filename == "archbird/release.json":
                document = json.loads(data)
                document["source_commit"] = "0" * 40
                data = (
                    json.dumps(document, indent=2, sort_keys=True) + "\n"
                ).encode("utf-8")
            target.writestr(info, data)
    _run(
        *check_command[:-3],
        str(forged),
        str(sdist),
        str(npm),
        cwd=root,
        success=False,
        contains="release provenance commit differs",
    )

    stage = repository / "tools/stage_release_provenance.py"
    _run(sys.executable, str(stage), "check", cwd=repository)
    _run(sys.executable, str(stage), "python", cwd=repository)
    if not (repository / "py/archbird/release.json").is_file():
        raise AssertionError("Python release provenance was not staged")
    _run(sys.executable, str(stage), "python", "--clean", cwd=repository)

    _run(sys.executable, str(stage), "node", cwd=repository)
    staged_package = (repository / "js/package.json").read_bytes()
    if json.loads(staged_package).get("gitHead") != commit:
        raise AssertionError("Node package did not receive exact source commit")
    (repository / "js/package.json").write_bytes(staged_package + b" ")
    _run(
        sys.executable,
        str(stage),
        "node",
        "--clean",
        cwd=repository,
        success=False,
        contains="changed during release staging",
    )
    (repository / "js/package.json").write_bytes(staged_package)
    _run(sys.executable, str(stage), "node", "--clean", cwd=repository)
    if (repository / "js/package.json").read_bytes() != package_bytes:
        raise AssertionError("Node release cleanup did not restore package.json")

    (repository / "js/package.json").write_bytes(package_bytes + b" ")
    _run(
        sys.executable,
        str(stage),
        "check",
        cwd=repository,
        success=False,
        contains="clean tracked worktree",
    )
    (repository / "js/package.json").write_bytes(package_bytes)
    (repository / "untracked-source.py").write_text(
        "print('not in release tag')\n",
        encoding="utf-8",
    )
    _run(
        sys.executable,
        str(stage),
        "check",
        cwd=repository,
        success=False,
        contains="unexpected untracked paths",
    )
    (repository / "untracked-source.py").unlink()
    (repository / "after-tag.txt").write_text("new commit\n", encoding="utf-8")
    _git(repository, "add", "after-tag.txt")
    _git(repository, "commit", "-qm", "after tag")
    _test_transition_checker(repository, root)
    _run(
        sys.executable,
        str(stage),
        "check",
        cwd=repository,
        success=False,
        contains="points to",
    )


def main() -> None:
    test_native_contract_mutations()
    test_streaming_contract_mutations()
    temporary_root = REPOSITORY / "build/tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as raw:
        root = Path(raw)
        test_documentation_contract_mutation(root)
        test_release_provenance_and_attestation(root)
    print("release gate mutation tests passed")


if __name__ == "__main__":
    main()
