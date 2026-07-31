# Archbird for JavaScript

**Map codebases. Verify architecture. Plan and apply structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

```bash
npm install --save-dev archbird

npx archbird            # shorthand for: npx archbird map
npx archbird .          # shorthand for: npx archbird map .
npx archbird map        # explicit form
npx archbird plan       # derive edits from current constraint issues
npx archbird act PLAN.json # materialize and verify a Patch; writes nothing
npx archbird apply PATCH.json # replay the accepted Patch
npx archbird serve      # explore it in the local web application
```

## Stages

| Stage | Question | Output |
| --- | --- | --- |
| **Map** | What exists, and how is it connected? | Searchable files, symbols, dependencies, tests, and build routes |
| **Query** | Which exact evidence matters for this task? | Focused, ranked context with source witnesses |
| **Verify** | Does the code follow the architecture constraints? | Constraint status, violations, code locations, and unknowns |
| **Plan** | What exact edits follow from current evidence, and what remains unknown? | An editable source-locked Plan with operations and acceptance constraints |
| **Act** | What exact patch does the Plan produce, and does its after-state pass? | An accepted, sealed Patch bound to fresh Map and Verification evidence |

Every result links back to the source, configuration, or test data used to
produce it. Missing or uncertain information is shown instead of guessed.

Map works without configuration. Add Verify when you want automated
architecture constraints. Plan derives only edits established by current
evidence and exposes underdetermined work as manual items. Act materializes and
checks exact edits against an isolated after-state without writing. Apply
revalidates source locks and replays only an accepted Patch.

`npx archbird` and `npx archbird .` remain supported shortcuts for mapping the
current repository. The explicit `npx archbird map` form is useful in scripts
and alongside the other stage commands.

## Command line

Start with the CLI in any repository; no configuration is required:

```bash
cd project
npx archbird
npx archbird query --symbol runtime_start
npx archbird query --search 'provider registry'
npx archbird serve
```

### Save and reuse evidence

Save complete evidence when subsequent operations must use the exact same
repository state:

```bash
mkdir -p .archbird
npx archbird map . --format json --pretty \
  --output .archbird/map.json --check
npx archbird config show . --pretty \
  --output .archbird/resolution.json --check

npx archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000

npx archbird query --map .archbird/map.json \
  --search 'provider registry' --search-limit 8

npx archbird impact --map .archbird/map.json \
  --path src/runtime.c --depth 2

npx archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' \
  --view changes --detail compact --check

npx archbird query --git-diff HEAD \
  --view changes --detail compact --check

npx archbird query --git-diff HEAD --view changes \
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
npx archbird map --view overview --detail compact
npx archbird map --view architecture \
  --group-by component --level file --relations imports,calls
npx archbird map --view tests --group-by directory
npx archbird map --view evidence --detail full
npx archbird query --symbol runtime_start --view source
npx archbird query --symbol runtime_start --dump
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
claims that a test ran. Use project-runner observations for executed routes.

Run the local application while source changes:

```bash
npx archbird serve
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.
Live Map, projection, Query, Verify, and source work runs in the native Node
host; the page receives typed ProjectionResults and does not load browser Wasm.
Normal exploration does not download the canonical Map; saving it is explicit.

## Configuration

Archbird works without config. Add `archbird.json` when names and boundaries
are reviewed project intent. CLI arguments override project config, which
overrides versioned discovery defaults.

```bash
npx archbird config show . --pretty
npx archbird config init . --output archbird.json
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
`src/**/*.c` is recursive. Components group selected files; they do not discover
new files. `route_to` is broad asserted intent; `case_routes` is case-specific.
Patterns use the pinned `archbird-pcre2-v1` contract rather than JavaScript
`RegExp`.

Root `compile_commands.json` and `index.scip` files are consumed automatically
in zero-config mode. Multiple compiler outputs can be named and kept separate:

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

Archbird consumes compiler outputs but never invokes a compiler or indexer.
Build routes expose repository source paths, compiler basenames, and command
digests without leaking absolute build-machine paths. SCIP facts retain their
variant, producer, source anchoring, coverage, and freshness.

The embedded config is mirrored by `examples/minimal.archbird.json`; the
complete multi-language form is `examples/quickstart.archbird.json` in the
source distribution.

The npm package exports the versioned JSON schemas for offline editors and
agents. For example,
`require.resolve("archbird/schema/archbird.schema.json")` locates the exact
project-configuration schema shipped with the installed engine. The native
configuration compiler remains authoritative for relational invariants that
standard JSON Schema cannot express.

## Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The staged configuration above
therefore needs no second suite file.

For a first check in an unfamiliar repository, configuration may contain only
the reviewed constraint; discovery supplies the project model and layers:

```bash
npx archbird verify --config - --check <<'JSON'
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
npx archbird query public-api-impact
npx archbird query --symbol demo_open --direction upstream

# Evaluate the whole reviewed policy or one named constraint.
npx archbird verify --check
npx archbird verify CORE-PUBLIC-API --check

# Preserve exhaustive repository-inventory operands when verifying a saved Map.
npx archbird verify --map .archbird/map.json \
  --resolution .archbird/resolution.json --check

# Emit CI-native reports from the same constraints.
npx archbird verify --format sarif --output .archbird/architecture.sarif --check
npx archbird verify --format junit --output .archbird/architecture.junit.xml --check

# Freeze reviewed existing debt and coverage as a ratchet.
npx archbird verify --freeze .archbird/architecture.baseline.json \
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
`npx archbird query . --symbol demo_open`.
`npx archbird impact ../project --path src/api.c` works similarly. A bare
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

Run each case in isolation with V8 or Istanbul coverage, then convert the
project-owned reports without rerunning the project:

```bash
npx archbird observe . --map .archbird/map.json \
  --request .archbird/coverage-request.json \
  --output .archbird/test-symbols.json
```

`compileTestObservations(map, request, { repository, requestDirectory })`
provides the same Node operation. Node also accepts isolated LLVM and gcov
JSON. Aggregate reports without exact per-test identity are rejected; use the
Python host for coverage.py dynamic contexts.

## Plan and Act

`plan` evaluates the complete current policy and produces one editable
source-locked artifact. `act` materializes its deterministic Patch, rebuilds the
isolated after-state, and emits only accepted bytes without writing. `apply`
revalidates and replays that Patch without reevaluating the Plan.

```bash
npx archbird plan --output .archbird/plan.json
npx archbird plan CORE-PUBLIC-API --output .archbird/plan.json
npx archbird plan CORE-PUBLIC-API --rename oldApi=newApi \
  --output .archbird/plan.json
npx archbird map --format json --output .archbird/before-map.json
# After a partial implementation/consumer migration:
npx archbird plan FFI-SURFACE --before-map .archbird/before-map.json \
  --output .archbird/plan.json
npx archbird act .archbird/plan.json
npx archbird act .archbird/plan.json --format patch
npx archbird act .archbird/plan.json --format json \
  --output .archbird/patch.json
npx archbird apply .archbird/patch.json
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
current Map. The TypeScript compiler provider proves JavaScript, TypeScript,
and TSX references while preserving aliased local names. Candidate or
unresolved calls, duplicate targets, and unsupported inputs block execution
instead of producing a partial rename.

For a `provider_surface` issue, the same reviewed rename can replace one stale
registration with a uniquely resolved surface member when the declaration
comes from one Make provider recorded in the canonical Map. The native compiler
proves one exact source-locked token match before emitting
`edit_make_variable_token`; it does not reopen project configuration.
Ambiguous, missing, duplicate, or non-Make cases remain manual.

Supplying `--before-map` allows the native compiler to finish one exact
observed provider-surface rename without a separate `--rename`. The old member
must have resolved uniquely before; exactly one new current member must retain
the same implementation paths and use ledger; and both declaration and
implementation signatures must differ only at the identifier. Both Maps must
share project, configuration, and producer identities. Incompatible,
diagnostic-bearing, signature-poor, or ambiguous histories remain
non-executable.

If the constraint itself requires an implemented and used surface member that
is not registered, Plan can derive `insert_make_variable_token` without an
extra flag. This requires exactly one configured Make provider and one unique
editable anchor token. Plan uses the direct or leading-underscore convention
proven by that declaration, and the longest canonical-name prefix preserves
locality. The item is `derived`; incomplete, ambiguous, or multi-provider
evidence stays manual.

Several missing members in the same Make provider remain separate Plan
obligations but materialize as one source-locked file transition. Only
distinct insertions sharing the same variable, anchor, and side compose;
tokens are ordered canonically and duplicate edits remain conflicts.

Plan can also derive removal of one unresolved, unused Make registration when
the Map proves zero candidates, zero uses, one exact declaration, and another
uniquely resolved declaration from the same provider. It will not empty a
configured provider, treat two stale entries as proof for each other, or turn
a mixed replacement into an inferred deletion.

Existing sources use SHA-256 locks; ranges use UTF-8 byte offsets and include
expected text. Manual items expose missing transformation inputs and block Act
instead of inventing code. Act evaluates the complete prepared file set
through `Project.withSourceOverlay()`, deriving a fresh Map and every
source-policy constraint before the first write. Incomplete relation evidence
blocks destructive generation. Failed, unknown, or unsatisfied fresh
acceptance writes nothing; only a satisfied after-state emits an accepted
Patch. Apply then advances through source-lock revalidation and transactional
replay. Plan input is bounded to
64 MiB, collections and touched files to 4,096, individual source files and
patches to 64 MiB, and aggregate touched source and patch output to 256 MiB.
Project compilers and tests remain external; their reviewed observations can
participate in Verify.

## Runtime and language evidence

The npm package has no install or postinstall compiler hook. It uses a matching
Linux x64 glibc Node-API prebuild when available and otherwise the bundled Wasm
core. `npm run build:native` is an explicit source build. Select or inspect the
engine with:

```bash
ARCHBIRD_ENGINE=native npx archbird support --pretty
ARCHBIRD_ENGINE=wasm npx archbird map . --check
```

| Language | npm/Node provider | Browser provider |
| --- | --- | --- |
| JavaScript/TypeScript/TSX | TypeScript compiler + Tree-sitter + lexical | TypeScript compiler + Tree-sitter + lexical |
| C/C++ | Tree-sitter + lexical | Tree-sitter + lexical |
| Python | Tree-sitter + lexical | Tree-sitter + lexical |
| R | Tree-sitter + lexical | Tree-sitter + lexical |

For CPython-AST evidence, use the PyPI host. Tree-sitter recovery is fact-local;
SCIP retains producer, document coverage, source anchoring, and freshness.
Provider conflicts, ambiguity, and unresolved targets remain explicit.

Node's per-file provider facts and materialized complete unchanged Maps are
content-addressed and revalidated against the native/Wasm core, configuration,
selected source bytes, and provider implementations. Both tiers share a 1 GiB
budget and evict the oldest entries;
`--cache-max-bytes` or `ARCHBIRD_CACHE_MAX_BYTES` changes it, `--cache-dir`
selects the root, and `--no-cache` disables it. Active and unverifiable cache
temporaries are preserved; abandoned same-execution-domain writes are removed
on the next use. Ownership includes the boot and PID-namespace domain where
available. A full cache warns without changing canonical output.

## Visualization, interchange, and commands

```bash
npx archbird export json --map .archbird/map.json --view components \
  --output .archbird/components.json
npx archbird export graphml --map .archbird/map.json \
  --output .archbird/architecture.graphml
npx archbird export mermaid --map .archbird/map.json \
  --output .archbird/architecture.mmd
```

Canonical Archbird JSON is authoritative. Graph-view JSON drives the app;
GraphML and Mermaid are deterministic projections. Node exposes normalized OKF
publication primitives, but the filesystem OKF CLI is Python-only. SCIP is an
input evidence provider. Verification results can render SARIF or JUnit; Plan
and Patch remain canonical JSON artifacts.

<!-- archbird-node-cli:start -->
The CLI command names are `map`, `config`, `query`, `impact`, `diff`,
`observe`, `freshness`, `workspace`, `verify`, `plan`, `act`, `apply`, `export`,
`serve`, and `support`.
<!-- archbird-node-cli:end -->

`config` provides `show|init`; `export` provides
`json|graphml|mermaid`. Use
`npx archbird COMMAND --help` for flags. Exit status is 0 for success, 1 when
requested `--check` blocks, and 2 for invalid input or configuration.

## JavaScript APIs

### Node

```js
const {
  Project,
  auditMapFreshness,
  compilePlan,
} = require("archbird");

const project = Project.fromRepository(".");
try {
  const mapJson = project.mapJson({ pretty: true });
  console.log(project.mapMarkdown({ maxChars: 12000 }).toString("utf8"));
  console.log(project.queryMarkdown({
    symbols: ["src/runtime.c:runtime_start"],
    depth: 1,
    context: { profile: "change" },
  }).toString("utf8"));
  const selectionJson = project.queryJson({
    symbols: ["src/runtime.c:runtime_start"],
    depth: 0,
  });
  console.log(project.sourceMarkdown({
    artifactJson: selectionJson,
  }).toString("utf8"));
  console.log(auditMapFreshness(mapJson, project.mapJson()).toString("utf8"));
  if (project.verificationConfigured) {
    const verificationJson = project.verifyJson();
    const planJson = compilePlan(
      project,
      project.mapJson(),
      verificationJson,
    );
    console.log(JSON.parse(planJson.toString("utf8")).artifact);
  }
} finally {
  project.dispose();
}
```

`Project.fromRepository()` applies discovery, project configuration, and
explicit options. `Project.fromConfig()` requires one reviewed configuration.
Canonical JSON methods return stable artifact bytes; Markdown and graph outputs
are presentation views.

`compilePlan()` delegates Plan derivation to the native core. Its optional
`beforeMapJson` input enables identity-checked residual planning; Node performs
no Map comparison or action inference.
`materializePatch()` produces exact binary-safe transitions from a Plan;
`acceptPatch()` seals them only after callers supply the fresh isolated
after-Map and Verification. `preflightPatchApply()` checks the accepted Patch
against newly observed source preimages immediately before a host replays its
stored bytes. The explicit filesystem helpers `observePlanSources()`,
`patchOverlay()`, `renderPatch()`, and `applyAcceptedPatch()` provide that host
transport; all Plan interpretation, edit materialization, and acceptance
remain in the native core.

<!-- archbird-node-api:start -->
| Area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map and Query | `analyzeWorkspace`, `auditMapFreshness`, `diffMaps`, `exportGraph`, `queryMap`, `queryMapMarkdown`, `renderMapMarkdown`, `renderSourceMarkdown`, `resolveDiscovery` |
| Projection and policy | `compileProjectConfiguration`, `compileQueryPlan`, `evaluateConstraints`, `evaluateProjection`, `freezeConstraints`, `reportConstraints` |
| Plan, Act, and Patch | `acceptPatch`, `actSourceRequirements`, `applyAcceptedPatch`, `compilePlan`, `materializePatch`, `observePatchSources`, `observePlanSources`, `patchOverlay`, `patchSourceRequirements`, `preflightPatchApply`, `renderPatch`, `validatePatch`, `validatePlan` |
| Observations and OKF | `analyzeOkfSource`, `compileTestObservations`, `publishOkfBundle` |
| Runtime and planning | `defaultProviderCacheDir`, `defaultProviderCacheMaxBytes`, `discoveryPlan`, `jsonCanonicalize` |
| Runtime metadata | `ENGINE`, `IMPLEMENTATION_SHA256`, `NATIVE_ABI_VERSION`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `PROVIDER_SUPPORT`, `VERSION` |
<!-- archbird-node-api:end -->

The inventory above is checked against `Object.keys(require("archbird"))`.
Node and Python expose parallel stage operations while retaining host-specific
runtime, cache, schema, OKF, and observation helpers.

### Browser and package entrypoints

```js
const { createBrowserArchbird } = require("archbird/browser");

const archbird = await createBrowserArchbird();
const project = archbird.Project.fromFiles([
  new archbird.Source(
    "src/index.ts",
    new TextEncoder().encode("export function answer() { return 42; }\n"),
  ),
]);
try {
  console.log(project.map());
  const selectionJson = project.queryJson({ symbols: ["answer"], depth: 0 });
  console.log(JSON.parse(selectionJson.toString("utf8")));
  console.log(project.sourceMarkdown({
    artifactJson: selectionJson,
  }).toString("utf8"));
} finally {
  project.dispose();
}
```

Browser input is supplied bytes; it has no filesystem discovery. The resolved
facade is:

<!-- archbird-browser-api:start -->
`Project`, `Source`, `auditMapFreshness`, `ENGINE`, `NATIVE_ABI_VERSION`,
`PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `VERSION`, and `core`.
<!-- archbird-browser-api:end -->

The `core` property is the advanced raw Wasm facade.

<!-- archbird-node-entrypoints:start -->
Package entrypoints are `archbird`, `archbird/browser`, `archbird/schema/*`,
`archbird/serve`, `archbird/wasm`, `archbird/wasm-sync`, and
`archbird/worker`.
<!-- archbird-node-entrypoints:end -->

The direct browser API runs in the caller; the worker entrypoint and application
isolate analysis in a Web Worker.

## Guarantees and limits

- Identical selected source, config, provider implementations, and supplied
  evidence produce byte-identical canonical output under the same Archbird
  implementation.
- Source providers do not import or execute analyzed packages; Verify evaluates
  typed predicates rather than asking a model to judge architectural truth.
- Static routes are navigation evidence, not runtime execution or coverage.
- Lexical/syntax evidence is not whole-program semantic resolution.
- Dynamic dispatch/reflection, C preprocessing, complete Make evaluation, ABI
  layout, and arbitrary generated code need stronger supplied evidence or
  remain unknown.
- Archbird is pre-1 software; schemas and ABI can evolve under semantic
  versioning without a 1.x compatibility promise.

Requires Node 18+. Archbird is Apache-2.0 licensed. Content-hashed JSON schemas
ship in the npm package; the C/Python hosts are included in the source
repository. This README is the complete npm/Node/browser usage contract.
