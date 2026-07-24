# Archbird

**Map codebases. Verify architecture. Plan and check structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

[![PyPI](https://img.shields.io/pypi/v/archbird)](https://pypi.org/project/archbird/)
[![npm](https://img.shields.io/npm/v/archbird)](https://www.npmjs.com/package/archbird)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

[JavaScript documentation](https://archbird.org/js/) ·
[Python documentation](https://archbird.org/py/) ·
[Open the browser app](https://archbird.org/app/)

```bash
# Python 3.9+
python -m pip install archbird

# Or Node 18+
npm install --save-dev archbird

archbird                # shorthand for: archbird map
archbird .              # shorthand for: archbird map .
archbird map            # explicit form; npm: npx archbird map
archbird serve          # npm: npx archbird serve
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
architecture constraints. Use Act when one change spans several files, packages, or
languages and you want to record the required outcome and check it afterward.
Archbird never edits the code.

`archbird` and `archbird .` remain supported shortcuts for mapping the current
repository. The explicit `archbird map` form is useful in scripts and alongside
the other stage commands.

## Map and query a repository

Start in any repository. No configuration or saved artifact is required:

```bash
cd project
archbird
archbird query --symbol runtime_start
archbird query --search 'where is provider registration handled'
archbird serve
```

The first command prints an architecture overview. Query derives a focused
artifact from the same canonical Map model; `serve` opens the local application.
Add selectors such as `--path`, `--component`, or `--test` as the question
becomes more specific.

### Save and reuse evidence

Save complete evidence when subsequent operations must use the exact same
repository state:

```bash
mkdir -p .archbird
archbird map . --format json --pretty \
  --output .archbird/map.json --check

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000 --check

archbird query --map .archbird/map.json \
  --search 'where is provider registration handled' --search-limit 8

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

Query selectors accept exact or partial symbols, paths, mapped directories,
globs, layers, components, tests, packages, artifacts, builds, provider
surfaces, parity surfaces, and named entries. `query` is bidirectional by
default; `impact` starts upstream. Occurrence-backed symbol relations are used
before conservative file expansion.

`--search TEXT` is the deterministic starting point when you do not yet know a
path or symbol. It ranks candidate symbols, files, components, packages, and
artifacts from names, paths, signatures, component descriptions, and package
metadata, shows the exact field and match behind every score, then expands the
selected candidates through the same typed graph. Prefix, substring, and
bounded typo matches are advisory seeds; they never become semantic edges or
make a constraint pass. Symbol neighbors reached only from those advisory
seeds do not strengthen a static test route.

Focused test routes keep file distance and symbol-hop distance separate. A
case that calls a wrapper reached from the requested symbol is labeled with
that wrapper and its hop distance; another case in the same file is not
promoted merely because the file is nearby. Static routes remain candidates
until reviewed configuration or imported per-test observations provide
stronger evidence.

`query --view changes` uses the same complete Query artifact and ranking policy,
but presents it as a coding packet: change seeds, affected code, strongest
routes, ranked tests, packages/builds/artifacts, explicit uncertainty, and a
ledger of collapsed evidence. It does not infer an edit or change the canonical
Query.

`--git-diff REVISION` asks the host CLI for Git's tracked name/status changes
and passes a typed change set to the same core Query operation. Current paths
seed the architecture neighborhood; deletions and paths outside the Map remain
visible instead of being invented as current code. External diff drivers and
text-conversion commands are disabled. Untracked files require an explicit
`--path` until they enter Git's diff.

`--verification-result PATH` adds the architecture constraints and findings
whose subject-side source paths overlap the selected change. The brief shows
their constraint IDs, requirement IDs, owners, severity, source paths, and whether
the verification input and producer still match the current Map. It does not
rerun verification, match reference-only evidence, or treat a path-free constraint
as relevant.

The default is an architecture-first overview for a person or coding agent;
canonical JSON still contains every selected file and mapped fact. Choose the
human projection and its amount of detail independently:

```bash
archbird map --view overview --detail compact     # shortest repository brief
archbird map --view architecture                  # components and connections
archbird map --view audit --detail standard       # complete analysis accounting
archbird map --view audit --full                  # all human-readable Map detail
```

`--compact` and `--full` are aliases for the corresponding `--detail` values.
`--max-chars` is a final output guard; it never changes the canonical Map.
Query profiles (`exact`, `change`, `architecture`, `audit`), per-kind quotas,
route provenance/confidence, and candidate/conservative policies control which
focused evidence is shown.

Plain saved-Map queries accept every supported Map schema even when another
Archbird core produced the artifact. Add `--check` when the result will drive a
decision: it also requires the saved producer digest to match the active core.
That producer check does not establish live-source freshness; use `freshness`
for a new Map-to-repository comparison. C/API query requests use
`producer_policy: "compatible"|"current"`; every Query records the effective
policy and its `current`, `different`, or `unknown` producer classification.

Progress is adaptive: `--progress auto` updates one terminal line only when an
analysis takes long enough to notice and stays silent for pipes and agents.
Use `always` for logs or `never` for silence.

`direct`, `candidate`, and `conservative` are static evidence strengths, not
claims that a test ran. Zero error diagnostics means selected analysis
completed; unresolved calls, parser recovery, partial surfaces, and unsupported
coverage remain visible.

Audit a saved Map against live source before treating it as current:

```bash
archbird freshness . --snapshot .archbird/map.json \
  --output .archbird/freshness.json --check
```

## Local and offline visualization

```bash
archbird serve
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.
The application provides component/file/symbol views, typed edge filters,
search, exact source witnesses, focused Query controls, source views, snapshots,
and structural diffs.

The same application can run statically on GitHub Pages. A browser can open a
saved artifact, local directory, or ZIP and analyze supplied files through the
Wasm Worker without uploading source or requiring a server.

## Project configuration

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
  "schema_version": 2,
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

That first file changes only Map construction. Add a reusable exhaustive
projection, a named Query, and a constraint when the project is ready to make
reviewed architecture policy persistent:

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

These are top-level members of the same `archbird.json`; the fragment is shown
separately to make the progression visible. The complete configuration
vocabulary is:

<!-- archbird-config-fields:start -->
| Section | Purpose |
| --- | --- |
| `schema_version`, `project`, `description` | configuration format, stable project identity, and human context |
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
`src/**/*.c` is recursive. Components group selected files; they do not discover
new source. `route_to` records broad asserted intent; `case_routes` is
case-specific. Patterns use the pinned `archbird-pcre2-v1` contract rather than
Python `re` or JavaScript `RegExp`.

Zero-config discovery consumes conventional root `compile_commands.json` and
`index.scip` files when present. Declare multiple compiler outputs explicitly
and give each one a stable variant when a repository has CPU, CUDA, Wasm, or
other builds:

```json
{
  "builds": [
    {"name": "wasm-db", "kind": "compile_commands", "path": "build/wasm/compile_commands.json", "variant": "wasm"}
  ],
  "indexes": [
    {"name": "wasm-scip", "format": "scip", "path": "build/wasm/index.scip", "variant": "wasm"}
  ]
}
```

Archbird reads these artifacts; it never invokes the compiler or indexer.
Compilation routes retain repository source paths, the compiler basename, and
a command digest without publishing absolute build-machine paths. SCIP facts
retain their variant, producer, source anchoring, coverage, and freshness.

The block above is mirrored by
[`examples/minimal.archbird.json`](examples/minimal.archbird.json). A complete
native/Python/TypeScript/package/build/test example is
[`examples/quickstart.archbird.json`](examples/quickstart.archbird.json). The
Draft 2020-12 project-configuration schema is
[`schema/archbird.schema.json`](schema/archbird.schema.json). The native
configuration compiler is authoritative and additionally enforces relational
invariants that standard JSON Schema cannot express, such as `min <= max`.
A shared accepted/rejected corpus keeps the schema and native C, Python, Node,
and Wasm compilers aligned for their common contract. Its `schema_version`
applies only to `archbird.json`. Map, ProjectionResult, Query, Verification,
and change artifacts each carry an independent schema version and migration
schedule; Archbird has no global schema version.

## Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The complete
[`quickstart.archbird.json`](examples/quickstart.archbird.json) combines these
stages and needs no second suite file.

```bash
# Run one saved Query plan or an ad-hoc query.
archbird query public-api-impact
archbird query --symbol demo_open --direction upstream

# Evaluate the whole reviewed policy or one named constraint.
archbird verify --check
archbird verify CORE-PUBLIC-API --check

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
as whole-policy compliance. Unknown IDs are errors. Repository selection is
execution context: run in the project root or use `--root PATH`; an external
configuration uses `--config CONFIG --root PROJECT`. Query and Impact also
accept an unambiguous path-shaped positional root, such as
`archbird query . --symbol demo_open`.
`archbird impact ../project --path src/api.c` works similarly. A bare
positional token remains a saved query ID; use `./project` rather than
`project` when selecting a relative repository path.

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

Named projections are reusable configuration for constraints and queries, not a
separate required CLI stage. One-off primitive operands stay inline:

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

Static test routes are candidates. `observe` converts project-owned per-test
coverage reports into exact runtime test-to-symbol evidence:

```bash
archbird observe . --map .archbird/map.json \
  --request .archbird/coverage-request.json \
  --output .archbird/test-symbols.json

archbird query --symbol runtime_start \
  --test-symbol-observations .archbird/test-symbols.json
```

Python supports coverage.py JSON with dynamic test contexts; Node supports V8
JSON. Both hosts support isolated Istanbul, `llvm-cov export`, and gcov JSON.
Formats without per-test contexts require one isolated report per case.
Archbird checks source hashes and rejects aggregate coverage that cannot prove
which test produced a hit; it never runs the tests or coverage tools.

## Act: review and judge a change

Archbird does not make the edit. It turns one failed architecture rule into a
proposed change checklist, records the checklist after review, and compares the
before and after repository state to see whether the required changes happened.

```text
derived finding → derived proposal → asserted contract
       external person, agent, IDE, or codemod edits the repository
new derived/observed evidence → derived transition result
```

```bash
archbird verify --format json \
  --output .archbird/before.verification.json

archbird plan --verification .archbird/before.verification.json \
  --finding FINGERPRINT --format markdown \
  --output .archbird/change.task.md

archbird plan --verification .archbird/before.verification.json \
  --finding FINGERPRINT --output .archbird/change.proposal.json

archbird contract --proposal .archbird/change.proposal.json \
  --objective "Restore the reviewed public surface" \
  --owner core --rationale "Reviewed evidence and implementation strategy" \
  --preserve-all --output .archbird/change.contract.json

# An external executor edits, builds, and tests; then regenerate Verify.
archbird verify --format json \
  --output .archbird/after.verification.json

archbird verify-plan \
  --proposal .archbird/change.proposal.json \
  --contract .archbird/change.contract.json \
  --before-verification .archbird/before.verification.json \
  --after-verification .archbird/after.verification.json --check
```

Proposals separate required postconditions, evidence-backed candidates,
preserved constraints, coverage, and unknowns. Contracts are immutable asserted
review. Results distinguish satisfied, missing, unexpected, unknown, stale,
and superseded. Candidate paths are advice, not write authorization. Markdown
task packets show the first 20 evidence rows by default; add `--full` when the
complete evidence list is needed.

## Evidence providers

Archbird stores normalized facts, not parser-specific trees. Several providers
can contribute without erasing provenance or blindly unioning contradictions.

| Level | Evidence | Providers |
| --- | --- | --- |
| L0 | paths, bytes, hashes, manifests, coverage | shared C/Wasm core |
| L1 | declarations, calls, test/build/FFI strings | portable lexical/protocol providers |
| L2 | syntax, scopes, imports, call shapes, spans, recovery | pinned Tree-sitter C, C++, Python, JS, TS/TSX, R |
| L3 | resolved definitions, references, relationships | supplied SCIP; CPython AST; TypeScript compiler |
| L4 | behavior and exact runtime hits | project-owned observed artifacts |

| Language | PyPI | npm | browser/Wasm |
| --- | --- | --- | --- |
| C/C++ | Tree-sitter + lexical | Tree-sitter + lexical | Tree-sitter + lexical |
| Python | CPython AST + Tree-sitter + lexical | Tree-sitter + lexical | Tree-sitter + lexical |
| JavaScript/TypeScript/TSX | Tree-sitter + lexical | TypeScript compiler + Tree-sitter + lexical | TypeScript compiler + Tree-sitter + lexical |
| R | Tree-sitter + lexical | Tree-sitter + lexical | Tree-sitter + lexical |
| Vue | lexical | lexical | lexical |

SCIP is host-neutral. Tree-sitter recovery is fact-local. Semantic indexes
retain producer, document coverage, source anchoring, and freshness. Provider
conflicts, ambiguity, and unresolved targets remain explicit.

## Programmatic APIs

### Python

```python
from archbird import Project

project = Project.from_repository(".")
map_json = project.map_json(pretty=True)
print(project.map_markdown(max_chars=12_000).decode())
print(project.query_markdown(symbols=["runtime_start"], depth=1).decode())
print(project.query_markdown(
    symbols=["runtime_start"], depth=1, view="changes", detail="compact"
).decode())
```

### JavaScript / Node

```js
const { Project } = require("archbird");

const project = Project.fromRepository(".");
try {
  console.log(project.mapMarkdown({ maxChars: 12000 }).toString("utf8"));
  console.log(project.queryMarkdown({
    symbols: ["runtime_start"], depth: 1, view: "changes", detail: "compact",
  }).toString("utf8"));
} finally {
  project.dispose();
}
```

### Browser

```js
const { createBrowserArchbird } = require("archbird/browser");

const archbird = await createBrowserArchbird();
const project = archbird.Project.fromFiles([
  new archbird.Source(
    "src/index.ts",
    new TextEncoder().encode("export const answer = 42;\n"),
  ),
]);
try {
  console.log(project.map());
} finally {
  project.dispose();
}
```

### C

```c
#include <archbird/archbird.h>

ArchbirdEngine *engine = NULL;
ArchbirdStatus status = archbird_engine_create(NULL, &engine);
if (status == ARCHBIRD_OK) {
  /* Supply repository-relative sources or normalized provider facts, then
     call Map, Verify, Query, Diff, workspace, or change APIs. */
  archbird_engine_destroy(engine);
}
```

The public C ABI uses opaque handles, allocator-aware byte buffers, explicit
statuses, and canonical JSON boundaries. It is experimental ABI v0.

### Complete API inventory

Python and Node share 31 top-level capabilities after conventional
snake_case/camelCase naming. The inventories below are checked against
`archbird.__all__` and `Object.keys(require("archbird"))`. Their totals
intentionally differ: Python adds schema readers, filesystem OKF output, and
standalone observation validation; Node adds engine/provider/cache diagnostics,
raw discovery/canonicalization, and a separate constraint-report renderer.
Python folds report rendering into
`evaluate_constraints_json(format=...)`.

<!-- archbird-python-api:start -->
| Python area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map and Query | `analyze_workspace_json`, `audit_map_freshness`, `diff_maps_json`, `export_graph`, `query_map_json`, `query_map_markdown`, `render_map_markdown`, `resolve_discovery` |
| Projection and policy | `compile_project_configuration`, `compile_query_plan_json`, `evaluate_constraints_json`, `evaluate_projection_json`, `freeze_constraints_json` |
| Change lifecycle | `ChangeContract`, `ChangeProposal`, `change_contract`, `change_proposal`, `change_verify` |
| Observations and OKF | `analyze_okf_source`, `compile_test_observations`, `export_okf_bundle`, `publish_okf_bundle`, `validate_test_symbol_observations`, `write_okf_bundle` |
| Runtime and schemas | `__version__`, `implementation_digest`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `read_schema`, `schema_names` |
<!-- archbird-python-api:end -->

<!-- archbird-node-api:start -->
| Node area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map and Query | `analyzeWorkspace`, `auditMapFreshness`, `diffMaps`, `exportGraph`, `queryMap`, `queryMapMarkdown`, `renderMapMarkdown`, `resolveDiscovery` |
| Projection and policy | `compileProjectConfiguration`, `compileQueryPlan`, `evaluateConstraints`, `evaluateProjection`, `freezeConstraints`, `reportConstraints` |
| Change lifecycle | `ChangeContract`, `ChangeProposal`, `compileChangeProposal`, `createChangeContract`, `verifyChangeContract` |
| Observations and OKF | `analyzeOkfSource`, `compileTestObservations`, `publishOkfBundle` |
| Runtime and planning | `defaultProviderCacheDir`, `defaultProviderCacheMaxBytes`, `discoveryPlan`, `jsonCanonicalize` |
| Runtime metadata | `ENGINE`, `IMPLEMENTATION_SHA256`, `NATIVE_ABI_VERSION`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `PROVIDER_SUPPORT`, `VERSION` |
<!-- archbird-node-api:end -->

`archbird/browser` exports `createBrowserArchbird()`. Browser repository input
is supplied bytes, not filesystem discovery. The resolved facade is:

<!-- archbird-browser-api:start -->
`Project`, `Source`, `auditMapFreshness`, `ENGINE`, `NATIVE_ABI_VERSION`,
`PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `VERSION`, and `core`.
<!-- archbird-browser-api:end -->

The `core` property is the advanced raw Wasm facade.

<!-- archbird-node-entrypoints:start -->
npm package entrypoints are `archbird`, `archbird/browser`,
`archbird/schema/*`, `archbird/serve`, `archbird/wasm`,
`archbird/wasm-sync`, and `archbird/worker`.
<!-- archbird-node-entrypoints:end -->

The complete C ABI is declared in
[`include/archbird/archbird.h`](include/archbird/archbird.h):

<!-- archbird-c-api:start -->
| C area | Public functions |
| --- | --- |
| Engine and JSON | `archbird_engine_create`, `archbird_engine_destroy`, `archbird_engine_error`, `archbird_engine_error_offset`, `archbird_engine_options_init`, `archbird_engine_options_init_for_input`, `archbird_graph_options_init`, `archbird_implementation_sha256`, `archbird_json_canonicalize`, `archbird_json_validate` |
| Discovery | `archbird_discovery_add_ignore`, `archbird_discovery_add_path`, `archbird_discovery_create`, `archbird_discovery_destroy`, `archbird_discovery_render`, `archbird_discovery_resolve`, `archbird_discovery_should_descend` |
| Configuration, projections, constraints | `archbird_constraints_evaluate`, `archbird_constraints_freeze`, `archbird_constraints_report`, `archbird_project_configuration_compile`, `archbird_projection_evaluate`, `archbird_query_plan_compile` |
| Project evidence | `archbird_project_add_provider_facts`, `archbird_project_add_source`, `archbird_project_add_test_symbol_observations`, `archbird_project_config_sha256`, `archbird_project_create`, `archbird_project_destroy`, `archbird_project_finalize_providers`, `archbird_project_finalize_sources`, `archbird_project_manifest_sha256`, `archbird_project_map_input_sha256`, `archbird_project_merge_summary`, `archbird_project_provider_count`, `archbird_project_provider_fact_count`, `archbird_project_render_file_facts`, `archbird_project_render_map`, `archbird_project_render_merge_conflicts`, `archbird_project_render_merge_ledger`, `archbird_project_render_provider_facts`, `archbird_project_scan_builtin`, `archbird_project_scan_builtin_provider`, `archbird_project_scan_builtin_provider_file`, `archbird_project_set_config`, `archbird_project_source`, `archbird_project_source_count`, `archbird_provider_facts_validate`, `archbird_source_manifest_validate`, `archbird_test_symbol_observations_validate` |
| Map, Query, interchange | `archbird_map_diff`, `archbird_map_export_graph`, `archbird_map_freshness`, `archbird_map_query`, `archbird_map_query_markdown`, `archbird_map_query_markdown_view`, `archbird_map_query_markdown_view_with_verification`, `archbird_map_render_markdown`, `archbird_map_render_markdown_view`, `archbird_okf_analyze`, `archbird_okf_publish` |
| Workspace and change | `archbird_change_contract`, `archbird_change_contract_report`, `archbird_change_proposal`, `archbird_change_proposal_report`, `archbird_change_verify`, `archbird_change_verify_report`, `archbird_workspace_analyze`, `archbird_workspace_plan` |
<!-- archbird-c-api:end -->

## Interchange and command surface

Canonical Archbird JSON is authoritative. Optional inputs/projections are:

| Format | Direction | Role |
| --- | --- | --- |
| SCIP | input | semantic definitions, references, relationships |
| OKF v0.1 | Python input/output; Node library projection | browsable knowledge bundle; prose never becomes constraints |
| graph-view JSON | output | typed interactive graph |
| GraphML, Mermaid | output | graph interchange and bounded diagrams |
| SARIF, JUnit | output | Verify/change CI integration |

```bash
archbird export json --map .archbird/map.json --view components \
  --output .archbird/components.json
archbird export graphml --map .archbird/map.json \
  --output .archbird/architecture.graphml
archbird export mermaid --map .archbird/map.json \
  --output .archbird/architecture.mmd
```

The command names are:

<!-- archbird-python-cli:start -->
Python: `map`, `config`, `query`, `impact`, `diff`, `observe`, `freshness`,
`workspace`, `verify`, `plan`, `contract`, `verify-plan`, `export`, `okf`,
`serve`, `support`.
<!-- archbird-python-cli:end -->

<!-- archbird-node-cli:start -->
Node: `map`, `config`, `query`, `impact`, `diff`, `observe`, `freshness`,
`workspace`, `verify`, `plan`, `contract`, `verify-plan`, `export`, `serve`,
`support`.
<!-- archbird-node-cli:end -->

`config` provides `show|init`; `export` provides `json|graphml|mermaid` and
Python additionally provides `okf`; Python `okf` provides
`validate|index|query`. Use
`archbird COMMAND --help` for flags. Exit status is 0 for success, 1 when
requested `--check` blocks, and 2 for invalid input/configuration.

Persistent caches are content-addressed and core-validated. Archbird reuses
per-file provider facts after a file changes and reuses a materialized complete
Map when the configuration, selected source bytes, providers, and core are all
unchanged. Both tiers share a 1 GiB default budget; use `--cache-max-bytes`,
`ARCHBIRD_CACHE_MAX_BYTES`, `--cache-dir`, or `--no-cache` to control storage.
Cache eviction or write failure never changes canonical analysis output.

## CI and agent workflow

```bash
archbird map . --format json --output .archbird/map.json --check
archbird verify \
  --format sarif --output .archbird/architecture.sarif --check
```

For agents:

1. Generate one checked canonical Map before broad exploration.
2. Run the reviewed constraint policy and start from stable constraint and
   requirement IDs.
3. Query bounded context and inspect the exact witnesses used for decisions.
4. Treat candidate/conservative tests as navigation, not execution.
5. Review every change contract before it becomes asserted intent.
6. Regenerate Map, runner evidence, and Verify after changes.
7. Check freshness before treating saved evidence as the live checkout.

## Guarantees, limits, and distribution

- Identical selected source, config, provider implementations, and supplied
  evidence produce byte-identical canonical output under the same Archbird
  implementation.
- Archbird performs no analyzed-project import/execution, repository mutation,
  network call, model call, codemod, or agent invocation.
- Lexical/syntax evidence is not whole-program semantic resolution; static test
  routes are not runtime execution or behavioral coverage.
- Dynamic dispatch/reflection, C preprocessing, complete Make evaluation, ABI
  layout, and arbitrary generated code need stronger supplied evidence or
  remain unknown.
- PyPI supplies CPython-AST precision and a CPython 3.10 manylinux x86-64 wheel;
  other supported Python/platform combinations build the included C snapshot.
- npm requires Node 18, has a Linux x64 glibc prebuild, and otherwise uses
  bundled Wasm unless native compilation is explicitly requested.
- Schemas and ABI are pre-1 and can evolve under semantic versioning.

Archbird is licensed under Apache-2.0. The shared core uses pinned yyjson,
PCRE2 10.47, Tree-sitter runtime, and grammar submodules under their upstream
licenses; source distributions retain the corresponding license files.

## Development

Clone the pinned third-party sources together with Archbird:

```bash
git clone --recurse-submodules --shallow-submodules \
  https://github.com/zemlyansky/archbird.git
cd archbird

# For an existing clone made without submodules:
git submodule update --init --recursive --depth 1

# Build and test the C core and shared library.
make native-test

# Build the live source frontends against build/libarchbird.so.
make build-py
./archbird map

make build-js
node js/src/cli.js .

# Complete local gates.
make test
make verify
make native-wasm-smoke
make app-test
```

Use `make editable-install PYTHON=/path/to/python` for Python source development
and `make build-c` after C edits. The root submodules are development inputs;
PyPI and npm releases contain generated, content-hashed C snapshots and never
require Git or submodules at installation time. `tools/sync_csrc.py` creates
those publishable snapshots from the pinned gitlinks; generated snapshots are
not a second source of truth.
