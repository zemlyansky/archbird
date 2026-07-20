# Archbird for Python

**Map codebases. Verify architecture. Plan and check structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

```bash
python -m pip install archbird

archbird .              # map the current repository
archbird serve .        # explore it in the local web application
```

## Map, Verify, Act

| Stage | Question | Output |
| --- | --- | --- |
| **Map** | What exists, and how is it connected? | Searchable files, symbols, dependencies, tests, and build routes |
| **Verify** | Does the code follow the architecture rules? | Passed and failed rules, with reasons and code locations |
| **Act** | Did a coordinated change produce the required structural result? | A change checklist and a before/after check |

Every result links back to the source, configuration, or test data used to
produce it. Missing or uncertain information is shown instead of guessed.

Map works without configuration. Add Verify when you want automated
architecture checks. Use Act when one change spans several files, packages, or
languages and you want to record the required outcome and check it afterward.
Archbird never edits the code.

## Use from Python

```python
from archbird import Project, audit_map_freshness

project = Project.from_repository(".")
map_json = project.map_json(pretty=True)                 # canonical bytes
overview = project.map_markdown(view="overview", detail="standard", max_chars=12_000)
context = project.query_markdown(
    symbols=["src/runtime.c:runtime_start"],
    depth=1,
    context={"profile": "change"},
    max_chars=8_000,
)
brief = project.query_markdown(
    symbols=["src/runtime.c:runtime_start"],
    depth=1,
    view="changes",
    detail="compact",
)

print(overview.decode())
print(context.decode())
print(brief.decode())
print(audit_map_freshness(map_json, project.map_json()).decode())
```

`Project.from_repository()` applies discovery, repository configuration, and
explicit options. `Project.from_config()` uses exactly one reviewed config.
Canonical JSON methods return stable artifact bytes or decoded objects;
Markdown and graph views are derived presentations and are not verification
inputs.

Saved-Map helpers `query_map_json()` and `query_map_markdown()` accept
`producer_policy="compatible"` (default) or `"current"`. The current policy
rejects a missing or different core producer digest; it is independent of the
live-source comparison performed by `audit_map_freshness()`.
`query_map_markdown(..., view="changes", verification_result=result_json)`
adds only checks with exact subject-side source-path overlap and reports input
and producer freshness.

Workspace, Verification, ChangeProposal, ChangeContract, change-result,
graph-view, freshness, diff, OKF, and report functions use the same canonical
schemas as the CLI and C core.

### Import observed test routes

Generate coverage.py JSON with pytest dynamic contexts, then convert it without
rerunning the project:

```bash
pytest --cov=your_package --cov-context=test
coverage json --show-contexts -o .archbird/coverage.json
archbird observe . --map .archbird/map.json \
  --request .archbird/coverage-request.json \
  --output .archbird/test-symbols.json
```

The request explicitly maps test selectors and contexts to report files and
names mapped runner configuration. `compile_test_observations()` provides the
same operation through Python. The Python host also accepts isolated Istanbul,
LLVM, and gcov JSON; use the Node host for V8’s UTF-16 offsets. Aggregate
reports without an exact per-test context are rejected rather than presented
as test attribution.

## Command line

Save complete evidence once and query it without re-analyzing:

```bash
mkdir -p .archbird
archbird map . --format json --pretty \
  --output .archbird/map.json --check

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000

archbird impact --map .archbird/map.json \
  --path src/runtime.c --depth 2

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' \
  --view changes --detail compact --check

archbird query . --git-diff HEAD \
  --view changes --detail compact --check

archbird query . --git-diff HEAD --view changes \
  --verification-result .archbird/verify.json --check
```

`query --view changes` presents the same complete Query artifact as a coding
packet. It groups change seeds, affected code, strongest routes, ranked tests,
packages/builds/artifacts, uncertainty, and collapsed evidence without
inventing an edit or changing canonical JSON.

`--git-diff REVISION` converts Git's tracked name/status output into a typed
change set. Current paths seed Query; deletions and paths outside the Map stay
explicit. External diff/text-conversion commands are disabled, and untracked
files require an explicit `--path`.

`--verification-result PATH` adds overlapping subject-side architecture checks
and findings, including requirement IDs and freshness. It does not rerun
verification or infer relevance from prose, reference-only facts, or checks
without exact source-path evidence.

Unchecked saved-Map queries accept supported older producers. Add `--check`
when the result will drive a decision; the shared core then requires the saved
producer digest to match the active core. Use `freshness` separately to compare
the saved source/config evidence with a newly derived live Map.

The default is an architecture-first overview. Canonical JSON contains every
selected file and mapped fact; Markdown is only a human projection:

```bash
archbird . --view overview --detail compact
archbird . --view architecture
archbird . --view audit --detail standard
archbird . --view audit --full
```

`--compact` and `--full` alias the corresponding detail levels. Query context
uses the `exact`, `change`, `architecture`, and `audit` profiles plus per-kind
quotas, route provenance/confidence, candidate/conservative policy, and finally
`--max-chars` as a guard. `--progress auto` updates one terminal line for long
interactive runs and stays silent when output is piped; use `always` or `never`
to override it.

`direct`, `candidate`, and `conservative` are static evidence strengths, not
claims that a test ran. Audit saved evidence before relying on it:

```bash
archbird freshness . --snapshot .archbird/map.json \
  --output .archbird/freshness.json --check
```

Run the local application while source changes:

```bash
archbird serve . --config archbird.json
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.

## Configuration

Archbird works without config. Add `archbird.json` when names and boundaries
are reviewed project intent. CLI arguments override project config, which
overrides versioned discovery defaults.

```bash
archbird config show . --pretty
archbird config init . --output archbird.json
```

`config init` is a review candidate, not architecture truth.

<!-- archbird-minimal-project-config:start -->
```json
{
  "schema_version": 1,
  "project": "demo",
  "layers": [
    {
      "name": "core",
      "role": "core",
      "language": "c",
      "globs": ["include/**/*.h", "src/**/*.c"],
      "public_headers": ["include/demo.h"]
    },
    {
      "name": "javascript",
      "role": "frontend",
      "language": "typescript",
      "globs": ["js/src/**/*.ts"]
    }
  ],
  "components": [
    {"name": "native-core", "paths": ["include/**", "src/**"]},
    {"name": "javascript-api", "paths": ["js/src/**"]}
  ]
}
```
<!-- archbird-minimal-project-config:end -->

Configuration can additionally declare:

| Section | Purpose |
| --- | --- |
| `packages`, `builds`, `artifacts` | manifests, public entrypoints, Autoconf/Make/npm routes, logical outputs and loaders |
| `bridges` | declared/used/implemented ABI, binding, or message surfaces |
| `tests` | static cases, reviewed `case_routes`, and generated-source relations |
| `named_entries`, `parity` | configured entrypoint protocols and reviewed surface relationships |
| `indexes` | one or more SCIP indexes with prefixes and position encoding |
| `checks`, `limits` | basic Map-presence requirements and bounded resource policy |

Selectors are segment-aware: `src/*.c` matches immediate children and
`src/**/*.c` is recursive. Components group selected files rather than
discovering new source. `route_to` is broad asserted intent; `case_routes` is
case-specific. Patterns use `archbird-pcre2-v1`, not Python `re`.

The embedded config is mirrored by `examples/minimal.archbird.json`; the
source repository also contains a complete package/build/test example in
`examples/quickstart.archbird.json`.

## Verify architecture

`architecture.verify.json` says which symbols, connections, surfaces, and test
routes must be present or absent. Archbird compares those rules with the
current map. Save this beside `archbird.json`:

<!-- archbird-minimal-verify-config:start -->
```json
{
  "schema_version": 1,
  "suite": "demo-architecture",
  "projects": {
    "subject": {"config": "archbird.json"}
  },
  "extractors": {
    "required.core_api": {
      "kind": "literal_set",
      "values": ["demo_close", "demo_open"]
    },
    "actual.core_api": {
      "kind": "symbols",
      "project": "subject",
      "layer": "core",
      "paths": ["include/demo.h"],
      "public_only": true
    }
  },
  "checks": [
    {
      "id": "CORE-PUBLIC-API",
      "assert": "required_subset",
      "expected": "required.core_api",
      "actual": "actual.core_api",
      "severity": "error",
      "owner": "core",
      "rationale": "Supported native entrypoints must remain public."
    }
  ]
}
```
<!-- archbird-minimal-verify-config:end -->

```bash
archbird verify --config architecture.verify.json --check
archbird verify --config architecture.verify.json \
  --format sarif --output .archbird/architecture.sarif --check
```

Verify supports set/value equality, mapped names/values, directional subsets,
cardinality, required/forbidden/allowed edges, acyclicity, minimum test routes,
and behavioral-attestation equality. Extractors cover literal facts, symbols,
values, component/file edges, test routes, provider surfaces, Python enums and
sets, C enums/designated initializers/macros, and supplied attestations.

Derived source facts, asserted requirements/mappings/waivers, and observed
runner evidence stay separate. Comparison, freshness, applicability,
disposition, and baseline state are independent; stale or unknown evidence
never becomes pass. Multi-project suites support source locks, explicit name
mappings, intentional supersets, reviewed divergences, and project-root
overrides that do not enter canonical evidence.

## Act: review and judge a change

Archbird does not make the edit. It turns one failed architecture rule into a
proposed change checklist, records the checklist after review, and compares the
before and after repository state to see whether the required changes happened.

```bash
archbird verify --config architecture.verify.json --format json \
  --output .archbird/before.verify.json

archbird plan --verification .archbird/before.verify.json \
  --finding FINGERPRINT --output .archbird/change.proposal.json

archbird contract --proposal .archbird/change.proposal.json \
  --objective "Restore the reviewed public surface" \
  --owner core --rationale "Reviewed evidence and implementation strategy" \
  --preserve-all --output .archbird/change.contract.json

# An external person, agent, IDE, or codemod edits and tests the repository.
archbird verify --config architecture.verify.json --format json \
  --output .archbird/after.verify.json

archbird verify-plan \
  --proposal .archbird/change.proposal.json \
  --contract .archbird/change.contract.json \
  --before-verification .archbird/before.verify.json \
  --after-verification .archbird/after.verify.json --check
```

Results distinguish satisfied, missing, unexpected, unknown, stale, and
superseded. Candidate paths are evidence-backed advice, not write authorization.
Relevant evidence changes invalidate a proposal; unrelated changes are context
drift.

## Interchange and visualization

```bash
archbird export json --map .archbird/map.json --view components \
  --output .archbird/components.json
archbird export graphml --map .archbird/map.json \
  --output .archbird/architecture.graphml
archbird export mermaid --map .archbird/map.json \
  --output .archbird/architecture.mmd
```

Canonical Archbird JSON is authoritative. Graph-view JSON drives the app;
GraphML and Mermaid are deterministic projections. Verify and change results
can render SARIF or JUnit.

The optional OKF adapter validates, indexes, queries, and publishes browsable
knowledge bundles while treating prose as context rather than executable truth:

```bash
python -m pip install 'archbird[okf]'
archbird okf validate knowledge/
archbird okf query knowledge/ --requirement ARCH-CORE-001
archbird export okf --map .archbird/map.json --output .archbird/knowledge
```

SCIP is a host-neutral input for semantic definitions, references, and
relationships. It needs no Python protobuf runtime; the `archbird[scip]` extra
is only for reference/differential tooling.

## Providers and runtime

| Language | Python host evidence |
| --- | --- |
| Python | CPython AST/symtable + Tree-sitter + lexical |
| C/C++ | Tree-sitter + lexical |
| JavaScript/TypeScript/TSX | Tree-sitter + lexical |
| R | Tree-sitter + lexical |
| Vue | lexical |
| supplied SCIP | native semantic decoder |

A Python file rejected by the installed CPython grammar marks that optional
provider inapplicable while portable facts remain. Tree-sitter recovery is
fact-local. SCIP retains producer, document coverage, source anchoring, and
freshness. Provider conflicts and unresolved targets remain explicit.

Per-file provider facts and materialized complete unchanged Maps are
content-addressed and revalidated against the native core, configuration,
selected source bytes, and provider implementations. The two tiers share a
1 GiB budget and evict the oldest content-addressed entries;
`--cache-max-bytes` or `ARCHBIRD_CACHE_MAX_BYTES` changes it, `--cache-dir`
selects the root, and `--no-cache` disables it. Failed temporaries are removed
on the next use, and a full cache produces a warning without invalidating the
analysis. `--jobs 0` is automatic. Python analysis uses a bounded ordered
process pool only for large Python source sets; worker count cannot change
canonical output.

## Commands, installation, and limits

The commands are `map`, `config show|init`, `query`, `impact`, `freshness`,
`diff`, `workspace`, `verify`, `plan`, `contract`, `verify-plan`,
`export json|graphml|mermaid|okf`, `okf validate|index|query`, `serve`, and
`support`. Use
`archbird COMMAND --help` for flags. Exit status is 0 for success, 1 when
requested `--check` blocks, and 2 for invalid input/configuration.

- Identical selected source, config, provider implementations, and supplied
  evidence produce byte-identical canonical output under the same Archbird
  implementation.
- Static routes are navigation evidence, not runtime execution or coverage.
- Lexical/syntax evidence is not whole-program semantic resolution.
- Dynamic dispatch/reflection, C preprocessing, complete Make evaluation, ABI
  layout, and arbitrary generated code need stronger evidence or remain unknown.
- Schemas and ABI are pre-1 and can evolve under semantic versioning.

The current wheel targets CPython 3.10 manylinux x86-64. Other supported
Python/platform combinations build the included content-hashed C snapshot and
need a C toolchain. The package has no required Python dependencies.

For editable source development, use:

```bash
make editable-install PYTHON=/path/to/environment/bin/python
make build-c   # after C edits
```

Archbird is Apache-2.0 licensed. This README is the complete PyPI/Python usage
contract; the strict JSON schemas and native C source ship in the source
repository and source distribution.
