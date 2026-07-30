# Archbird for Python

**Map codebases. Verify architecture. Plan and apply structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

```bash
python -m pip install archbird

archbird                # shorthand for: archbird map
archbird .              # shorthand for: archbird map .
archbird map            # explicit form
archbird plan           # derive edits from current constraint issues
archbird act PLAN.json  # preview; add --apply only after review
archbird serve          # explore it in the local web application
```

## Stages

| Stage | Question | Output |
| --- | --- | --- |
| **Map** | What exists, and how is it connected? | Searchable files, symbols, dependencies, tests, and build routes |
| **Query** | Which exact evidence matters for this task? | Focused, ranked context with source witnesses |
| **Verify** | Does the code follow the architecture constraints? | Constraint status, violations, code locations, and unknowns |
| **Plan** | What exact edits follow from current evidence, and what remains unknown? | An editable source-locked Plan with operations and acceptance constraints |
| **Act** | What patch does the Plan produce, and did its constraints pass? | A virtual patch or applied result bound to fresh Verification |

Every result links back to the source, configuration, or test data used to
produce it. Missing or uncertain information is shown instead of guessed.

Map works without configuration. Add Verify when you want automated
architecture constraints. Plan derives only edits established by current
evidence and exposes underdetermined work as manual items. Act previews without
writing; `archbird act PLAN.json --apply` first rebuilds Map and runs fresh
acceptance constraints over isolated after-state bytes, then commits only a
satisfied result.

`archbird` and `archbird .` remain supported shortcuts for mapping the current
repository. The explicit `archbird map` form is useful in scripts and alongside
the other stage commands.

## Command line

Start with the CLI in any repository; no configuration is required:

```bash
cd project
archbird
archbird query --symbol runtime_start
archbird query --search 'provider registry'
archbird serve
archbird mcp
```

### Save and reuse evidence

Save complete evidence when subsequent operations must use the exact same
repository state:

```bash
mkdir -p .archbird
archbird map . --format json --pretty \
  --output .archbird/map.json --check
archbird config show . --pretty \
  --output .archbird/resolution.json --check

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000

archbird query --map .archbird/map.json \
  --search 'provider registry' --search-limit 8

archbird impact --map .archbird/map.json \
  --path src/runtime.c --depth 2

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' \
  --view changes --detail compact --check

archbird query --git-diff HEAD \
  --view changes --detail compact --check

archbird query --git-diff HEAD --view changes \
  --verification-result .archbird/verify.json --check
```

Archbird excludes `.archbird/**` by default, so saving generated artifacts
there does not change repository discovery or freshness. Use
`--no-default-excludes` only when that tool-output tree is intentionally part
of the analyzed scope.

`--search KEYWORDS` is deterministic lexical retrieval, not natural-language
or semantic search. Use concise repository vocabulary such as
`provider registry`; every candidate records the matched field and match type.
Prefer a typed selector such as `--symbol` or `--path` once the target is known.

`query --view changes` presents the same complete Query artifact as a coding
packet. It groups change seeds, affected code, strongest routes, ranked tests,
packages/builds/artifacts, uncertainty, and collapsed evidence without
inventing an edit or changing canonical JSON.

`--git-diff REVISION` converts Git's tracked name/status output into a typed
change set. Current paths seed Query; deletions and paths outside the Map stay
explicit. External diff/text-conversion commands are disabled, and untracked
files require an explicit `--path`.

`--verification-result PATH` adds overlapping subject-side architecture constraints
and findings, including requirement IDs and freshness. It does not rerun
verification or infer relevance from prose, reference-only facts, or constraints
without exact source-path evidence.

Unchecked saved-Map queries accept supported older producers. Add `--check`
when the result will drive a decision; the shared core then requires the saved
producer digest to match the active core. Use `freshness` separately to compare
the saved source/config evidence with a newly derived live Map.

The default is an architecture-first overview. Canonical JSON contains every
selected file and mapped fact; Markdown is only a human projection:

```bash
archbird map --view overview --detail compact
archbird map --view architecture \
  --group-by component --level file --relations imports,calls
archbird map --view tests --group-by directory
archbird map --view evidence --detail full
archbird query --symbol runtime_start --view source
archbird query --symbol runtime_start --dump
```

`--view` chooses an overview, architecture, tests, or evidence preset.
`--group-by` independently organizes entities by directory, configured
component, layer, or language; `--level` selects component, file, or symbol
nodes; and repeatable comma-separated `--relations` overrides the preset.
These semantic axes compile to one exhaustive graph ProjectionPlan shared with
the application. `--detail` changes presentation density only; `--compact` and
`--full` are aliases, and `--max-chars` is a final rendering guard. Compact and
standard Markdown rank structural groups, aggregated provider-to-consumer
dependency flows, and file landmarks while accounting for presentation
omissions. Full detail enumerates the exhaustive selected records.

Graph completeness is distinct from repository coverage. Unsupported, ignored,
or oversized inputs remain an explicit coverage frontier without falsely
making a fully evaluated selected graph incomplete.

`--view source` materializes hash-checked source bytes for the Map or Query
selection. Compact detail is a declaration outline. Standard detail expands
exact symbol matches and complete directly selected paths while leaving
related files as outlines. Full detail returns every selected file; `--dump`
is an alias for `--view source --detail full` and cannot be combined with
`--max-chars`. Saved Maps do not contain source bytes, so use
`--root CHECKOUT` with a saved-Map source view. Archbird rejects changed bytes,
does not guess missing declaration extents, and does not embed non-UTF-8 or
terminal-control bytes in Markdown. Valid concrete-syntax boundaries outrank
semantic-AST boundaries for source rendering; alternate provider boundaries
remain recorded as merge variations.

Query context separately uses the `exact`, `change`, `architecture`, and
`audit` profiles plus per-kind quotas, route provenance/confidence, and
candidate/conservative policy. `--progress auto` updates one terminal line for
long interactive runs and stays silent when output is piped; use `always` or
`never` to override it.

`direct`, `candidate`, and `conservative` are static evidence strengths, not
claims that a test ran. Audit saved evidence before relying on it:

```bash
archbird freshness . --snapshot .archbird/map.json \
  --output .archbird/freshness.json --check
```

Run the local application while source changes:

```bash
archbird serve
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.
Live Map, projection, Query, Verify, and source work runs in the native Python
host; the page receives typed ProjectionResults and does not load browser Wasm.
Normal exploration does not download the canonical Map; saving it is explicit.

### Connect an agent over MCP

```bash
archbird mcp
archbird mcp --root ../project
archbird mcp --no-config
```

The MCP stdio server uses the same watched `LiveRepository` service as
`serve`, without tunneling through HTTP. It exposes bounded read-only status,
Map, projection, Query, hash-checked source, Verify, and Diff tools. Results
carry structured content and digest-bound resource links for retained Map
generations. Project configuration may come from `archbird.json` or a file;
stdin is reserved for the protocol.

See the official
[MCP stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [server tool/resource contract](https://modelcontextprotocol.io/specification/2025-11-25/server).

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

That first file changes only Map construction. Add a reusable projection, a
named Query, and a constraint when the project is ready to persist reviewed
architecture policy:

```json
{
  "projections": {
    "public-core-api": {
      "select": "symbols",
      "paths": ["include/demo.h"],
      "public_only": true
    }
  },
  "queries": {
    "public-api-impact": {
      "projection": "public-core-api",
      "direction": "upstream",
      "depth": 1
    }
  },
  "constraints": {
    "CORE-PUBLIC-API": {
      "assert": "required_subset",
      "expected": {"literal": ["demo_close", "demo_open"]},
      "actual": {"projection": "public-core-api"},
      "severity": "error",
      "owner": "core",
      "rationale": "Supported native entrypoints must remain public."
    }
  }
}
```

These are top-level members of the same `archbird.json`. The complete field
inventory is:

<!-- archbird-config-fields:start -->
| Section | Purpose |
| --- | --- |
| `project`, `description` | optional stable project identity and human context |
| `exclude`, `discovery` | project-level selection and explicit discovery policy |
| `layers`, `components` | selected source/provider groups and reviewed architecture groupings |
| `packages`, `builds`, `artifacts` | manifests, public entrypoints, compilation-database/Autoconf/Make/npm routes, logical outputs and loaders |
| `bridges` | declared/used/implemented ABI, binding, or message surfaces |
| `tests` | static cases, reviewed case routes, and generated-source relations |
| `named_entries`, `parity` | configured entrypoint protocols and reviewed surface relationships |
| `indexes` | one or more SCIP indexes with prefixes, position encoding, and build variants |
| `projections`, `queries`, `constraints` | reusable derivations, saved Query plans, and reviewed architecture policy |
| `limits` | bounded Map analysis policy |
<!-- archbird-config-fields:end -->

Selectors are segment-aware: `src/*.c` matches immediate children and
`src/**/*.c` is recursive. Components group selected files rather than
discovering new source. `route_to` is broad asserted intent; `case_routes` is
case-specific. Patterns use `archbird-pcre2-v1`, not Python `re`.

Root `compile_commands.json` and `index.scip` files are consumed automatically
in zero-config mode. Multiple compiler outputs can be named and kept separate:

```json
{
  "builds": [
    {"name": "cuda-db", "kind": "compile_commands", "path": "build/cuda/compile_commands.json", "variant": "cuda"}
  ],
  "indexes": [
    {"name": "cuda-scip", "format": "scip", "path": "build/cuda/index.scip", "variant": "cuda"}
  ]
}
```

Archbird consumes compiler outputs but never invokes a compiler or indexer.
Build routes expose repository source paths, compiler basenames, and command
digests without leaking absolute build-machine paths. SCIP facts retain their
variant, producer, source anchoring, coverage, and freshness.

The embedded config is mirrored by `examples/minimal.archbird.json`; the
source repository also contains a complete package/build/test example in
`examples/quickstart.archbird.json`.

## Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The staged configuration above
therefore needs no second suite file.

For a first check in an unfamiliar repository, configuration may contain only
the reviewed constraint; discovery supplies the project model and layers:

```bash
archbird verify --config - --check <<'JSON'
{
  "constraints": {
    "NO-LARGE-SOURCE": {
      "kind": "max_file_bytes",
      "include": ["src/**"],
      "max": 1048576,
      "owner": "architecture",
      "rationale": "Keep source files reviewable."
    }
  }
}
JSON
```

The same fragment can be saved unchanged as `archbird.json`. Explicit
project-model sections replace discovery; omitted sections inherit it.

```bash
# Run one saved Query plan or an ad-hoc query.
archbird query public-api-impact
archbird query --symbol demo_open --direction upstream

# Evaluate the whole reviewed policy or one named constraint.
archbird verify --check
archbird verify CORE-PUBLIC-API --check

# Preserve exhaustive repository-inventory operands when verifying a saved Map.
archbird verify --map .archbird/map.json \
  --resolution .archbird/resolution.json --check

# Emit CI-native reports from the same constraints.
archbird verify --format sarif --output .archbird/architecture.sarif --check
archbird verify --format junit --output .archbird/architecture.junit.xml --check

# Freeze reviewed existing debt and coverage as a ratchet.
archbird verify --freeze .archbird/architecture.baseline.json \
  --freeze-owner architecture \
  --freeze-rationale "Reviewed starting point"
```

`verify` without IDs evaluates every configured constraint. Positional IDs
select an explicit subset and the Verification artifact records configured,
requested, evaluated, and omitted counts; a successful subset is never reported
as whole-policy compliance. Unknown IDs are errors. The CLI defaults to a
human-readable Markdown verdict; use `--format json` when saving the canonical
Verification artifact. Repository selection is
execution context: run in the project root or use `--root PATH`; an external
configuration uses `--config CONFIG --root PROJECT`. Query and Impact also
accept an unambiguous path-shaped positional root, such as
`archbird query . --symbol demo_open`.
`archbird impact ../project --path src/api.c` works similarly. A bare
positional token remains a saved query ID; use `./project` rather than
`project` when selecting a relative repository path.

A saved Map contains mapped facts, not the complete discovery inventory.
Pass its matching `config show` artifact with `--resolution` when constraints
depend on ignored, unsupported, oversized, or forbidden repository paths.
Archbird validates the Map/resolution identities and rejects a mismatched pair.
Live `verify` derives both from one repository state automatically.

Common typed constraints cover required/forbidden paths and symbols, file-size
bounds, symbol cardinality, component membership and cycles, allowed/forbidden/
required component or file edges, package entrypoints, bridges, test routes, and
provider surfaces. They require no projection boilerplate. General predicates
cover set/value equality, mapped equality, directional subsets, cardinality,
numeric bounds, graph edges, acyclicity, minimum test routes, and observation
equality.

A projection result is exhaustive for its declared Map domain. If discovery,
provider, resource, freshness, or source-lock evidence prevents a complete
answer, the operand is partial or unknown and cannot make a constraint pass.
Query may rank and bound context; Verify may not. Derived Map facts, asserted
literals/mappings/waivers, and observed runner evidence retain distinct
provenance.

Named projections are useful when several constraints or queries share a
selection. One-off primitive operands stay inline:

```json
{
  "constraints": {
    "API-SIZE": {
      "assert": "cardinality",
      "actual": {
        "projection": {
          "select": "symbols",
          "paths": ["include/**"],
          "public_only": true
        }
      },
      "max": 30,
      "owner": "core",
      "rationale": "Keep the supported native surface reviewable."
    }
  }
}
```

Constraint-owned waivers require an ID, owner, rationale, an exact finding
fingerprint or comparison/key pair, and an expiry date or input-digest boundary.
Baselines classify new, known, reintroduced, and resolved findings while
ratcheting covered facts. Cross-repository constraints receive explicitly named
saved Maps with `--map-input ID=PATH`; behavioral parity receives reviewed
artifacts with `--observation ID=PATH`. Similar names alone never establish
semantic equivalence.

Every constraint has a stable ID, owner, rationale, optional requirement IDs,
tags and severity. Findings cite exact evidence and separately record
comparison, evidence state, applicability, disposition, baseline state, and a
stable fingerprint. JSON, Markdown, SARIF, and JUnit are views of the same
canonical Verification result.

### Add observed test evidence

Generate coverage.py JSON with pytest dynamic contexts, then convert it without
rerunning the project:

```bash
pytest --cov=your_package --cov-context=test
coverage json --show-contexts -o .archbird/coverage.json
archbird observe . --map .archbird/map.json \
  --request .archbird/coverage-request.json \
  --output .archbird/test-symbols.json
```

The request maps exact test selectors and contexts to report files.
`compile_test_observations()` provides the same operation through Python. The
Python host also accepts isolated Istanbul, LLVM, and gcov JSON; use the Node
host for V8 UTF-16 offsets. Aggregate reports without exact per-test identity
are rejected.

## Plan and Act

`plan` evaluates the complete current policy and produces one editable
source-locked artifact. `act` previews its deterministic patch without writing;
`--apply` explicitly writes only after the current Map and Verification match
the Plan source snapshot.

```bash
archbird plan --output .archbird/plan.json
archbird plan CORE-PUBLIC-API --output .archbird/plan.json
archbird plan CORE-PUBLIC-API --rename old_api=new_api \
  --output .archbird/plan.json
archbird act .archbird/plan.json
archbird act .archbird/plan.json --format patch
archbird act .archbird/plan.json --apply \
  --format json --output .archbird/act-result.json
```

Exact `replace_range`, `create_file`, `delete_file`, `move_file`,
`edit_json_pointer`, `edit_make_variable_token`,
`insert_make_variable_token`, and evidence-bound `rename_symbol` operations are
executable. An asserted `edit_json_pointer` operation changes one reviewed
manifest/export-table value under an exact source hash, RFC 6901 pointer, and
expected old JSON value without reformatting the complete file. An asserted
`edit_make_variable_token` operation replaces or removes one exact direct
token in a named Make variable while preserving its surrounding layout; stale,
missing, or duplicate matches block execution. An asserted
`insert_make_variable_token` adds a proven-absent direct token before or after
one unique anchor without guessing between assignments. A derived
one-extra/one-missing rename candidate is review evidence, not intent; it
remains non-executable until `--rename OLD=NEW` is supplied. The reviewed Plan
locks the exhaustive declaration/import/export/reference projection, and Act
requires the same result digest, completeness ledger, and sites against the
current Map. CPython AST/symtable evidence can prove direct imported-name
calls. Candidate or unresolved calls, duplicate targets, unsupported inputs,
and lexical-only C/C++ call binding block execution instead of producing a
partial rename.

Existing sources use SHA-256 locks; ranges use UTF-8 byte offsets and include
expected text. Manual items expose missing transformation inputs and block Act
instead of inventing code. Apply evaluates the complete prepared file set
through `Project.with_source_overlay()`, deriving a fresh Map and every
source-policy constraint before the first write. Incomplete relation evidence
blocks destructive generation. Failed, unknown, or unsatisfied fresh
acceptance writes nothing and remains visible as `failed` or `rejected`; only a
satisfied after-state advances to source-lock revalidation and transactional
commit. Plan input is bounded to
64 MiB, collections and touched files to 4,096, individual source files and
patches to 64 MiB, and aggregate touched source and patch output to 256 MiB.
Project compilers and tests remain external; their reviewed observations can
participate in Verify.

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
GraphML and Mermaid are deterministic projections. Verify and Act results
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

A Python file rejected by the installed CPython grammar marks that optional
provider inapplicable while portable facts remain. Tree-sitter recovery is
fact-local. SCIP retains producer, document coverage, source anchoring, and
freshness. Provider conflicts and unresolved targets remain explicit.

Per-file provider facts and materialized complete unchanged Maps are
content-addressed and revalidated against the native core, configuration,
selected source bytes, and provider implementations. The two tiers share a
1 GiB budget and evict the oldest content-addressed entries;
`--cache-max-bytes` or `ARCHBIRD_CACHE_MAX_BYTES` changes it, `--cache-dir`
selects the root, and `--no-cache` disables it. Active and unverifiable cache
temporaries are preserved; abandoned same-execution-domain writes are removed
on the next use when the host has a safe process-liveness probe; unverifiable
owners are retained. Ownership includes the boot and PID-namespace domain
where available. A full cache produces a warning without invalidating the analysis.
`--jobs 0` is automatic. CPython-AST analysis uses a bounded ordered supervised
process pool when more than one analyzer process is selected;
`--python-provider-timeout` bounds the wait for each ordered multiprocess source
batch and terminates its workers on failure or cancellation. Worker count and
timeout are host execution policy and cannot change canonical output.

## Python API

Use `Project` for the normal repository workflow:

```python
import json
from pathlib import Path

from archbird import (
    Project,
    audit_map_freshness,
    generate_plan,
    preview_plan,
)

project = Project.from_repository(".")
map_json = project.map_json(pretty=True)
overview = project.map_markdown(
    view="overview", detail="standard", max_chars=12_000
)
context = project.query_markdown(
    symbols=["src/runtime.c:runtime_start"],
    depth=1,
    context={"profile": "change"},
    max_chars=8_000,
)
selection = project.query_json(
    symbols=["src/runtime.c:runtime_start"], depth=0
)
source = project.source_markdown(
    artifact_json=selection
)

print(overview.decode())
print(context.decode())
print(source.decode())
print(audit_map_freshness(map_json, project.map_json()).decode())

if project.verification_configured:
    plan = generate_plan(
        project.map(),
        json.loads(project.verify_json()),
        None,
        Path("."),
    )
    print(preview_plan(plan, Path("."))["status"])
```

`Project.from_repository()` applies discovery, project configuration, and
explicit options. `Project.from_config()` requires one reviewed configuration.
Canonical JSON methods return stable artifact bytes; Markdown and graph outputs
are presentation views.

`apply_plan(plan, root, acceptance)` passes the acceptance callback a copied
Plan, the repository root, and an immutable `path -> bytes | None` source
overlay. Build its after-state with
`project.with_source_overlay(overlay, config=...)`; `None` represents a
deletion. Returning anything except complete satisfied acceptance prevents the
filesystem commit.

Saved-Map helpers `query_map_json()` and `query_map_markdown()` accept
`producer_policy="compatible"` or `"current"`. Configuration, projection,
QueryPlan, constraint, baseline, observation, workspace, Plan, Act, graph, and
OKF functions expose the same canonical artifacts as the CLI.

The optional ast-grep adapter is planning-time only. A reviewed integration
pins the executable SHA-256 and version, then
`materialize_ast_grep_operations()` translates a bounded non-mutating preview
into ordinary source-locked Plan operations. Act itself never invokes ast-grep.

`schema_names()` lists every bundled JSON schema and `read_schema()` returns its
exact bytes. Filesystem OKF parsing/writing is Python-specific because it uses
the optional YAML/CommonMark adapter; normalized OKF analysis and publication
remain shared with Node and C.

<!-- archbird-python-api:start -->
| Area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map and Query | `analyze_workspace_json`, `audit_map_freshness`, `diff_maps_json`, `export_graph`, `query_map_json`, `query_map_markdown`, `render_map_markdown`, `render_source_markdown`, `resolve_discovery` |
| Projection and policy | `compile_project_configuration`, `compile_query_plan_json`, `evaluate_constraints_json`, `evaluate_projection_json`, `freeze_constraints_json` |
| Plan and Act | `apply_plan`, `generate_plan`, `inspect_ast_grep_executable`, `materialize_ast_grep_operations`, `preview_plan` |
| Observations and OKF | `analyze_okf_source`, `compile_test_observations`, `export_okf_bundle`, `publish_okf_bundle`, `validate_test_symbol_observations`, `write_okf_bundle` |
| Runtime and schemas | `__version__`, `implementation_digest`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `read_schema`, `schema_names` |
<!-- archbird-python-api:end -->

## Commands, installation, and limits

<!-- archbird-python-cli:start -->
The command names are `map`, `config`, `query`, `impact`, `diff`, `observe`,
`freshness`, `workspace`, `verify`, `plan`, `act`,
`export`, `okf`, `serve`, `mcp`, and `support`.
<!-- archbird-python-cli:end -->

`config` provides `show|init`; `export` provides
`json|graphml|mermaid|okf`; `okf` provides `validate|index|query`. Use
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

Release wheels use the selected release interpreter and manylinux x86-64 tag.
Other supported Python/platform combinations build the included
content-hashed C snapshot and need a C toolchain. The package has no required
Python dependencies.

For editable source development, use:

```bash
make editable-install PYTHON=/path/to/environment/bin/python
make build-c   # after C edits
```

Archbird is Apache-2.0 licensed. This README is the complete PyPI/Python usage
contract; content-hashed JSON schemas and native C source ship in wheels and
source distributions.
