# Archbird for JavaScript

**Map codebases. Verify architecture. Plan and apply structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

```bash
npm install --save-dev archbird

npx archbird map .  # map the current repository
npx archbird serve  # explore it in the local web application
```

## Stages

| Stage | Question | Output |
| --- | --- | --- |
| **Map** | What exists, and how is it connected? | Searchable files, symbols, dependencies, tests, and build routes |
| **Query** | Which exact evidence matters for this task? | Focused, ranked context with source witnesses |
| **Path** | How are two explicit architecture entities connected? | Bounded shortest witnesses with typed relations, evidence, and completeness |
| **Verify** | Does the code follow the architecture constraints? | Constraint status, violations, code locations, and unknowns |
| **Plan** | What structural change follows from current evidence, and what remains unknown? | An editable language-neutral Plan with objectives, operators, applicability, and acceptance constraints |
| **Act** | How does that Plan ground into exact repository changes, and does its after-state pass? | An accepted, sealed Act bound to exact transitions plus fresh Map and Verification evidence |

**Map builds the canonical repository IR.** Architecture is normally scattered
across source languages, packages, public interfaces, native/frontend bridges,
tests, build systems, and generated artifacts. Map joins those facts once so
every later stage works from the same files, symbols, relationships, and source
locations. Provider-specific lexical, syntax, host-AST, and SCIP facts are
normalized into this language-neutral intermediate representation without
discarding their provenance or uncertainty.

**Query and Path evaluate projections over the Map IR.** Query selects,
traverses, and ranks the small neighborhood relevant to one task. Path answers
a narrower question: whether two explicit entities are connected and which
relationships form the connection. They share typed projection and graph
indexes, but neither operation rewrites the Map or promotes an uncertain
relationship into a confirmed one.

**Verify checks architectural intent against the complete relevant model.**
Each constraint compiles into exhaustive projections over the Map IR plus a
predicate such as required symbols, allowed dependencies, acyclicity, parity,
or a numeric bound. Verify never uses a ranked or truncated Query result. Its
Verification artifact records pass, failure, or unknown together with the exact
operands and source locations responsible.

**Plan describes a structural transition.** Archbird can derive a Plan from the
Map IR, current Verification findings, and a requested goal, but a developer or
agent may also author or edit one directly. The language-neutral Plan records
ordered objectives, applicable transformation operators, acceptance
constraints, source identities, and explicitly manual work; it does not contain
unreviewed guessed code.

**Act grounds and checks the Plan.** Act combines the reviewed Plan with exact
source bytes and executor input, produces candidate file transitions, and
evaluates their isolated after-state through a fresh Map and Verify run. A
passing result can be sealed as an accepted Act without touching the worktree.
Apply is the only mutating operation: it revalidates every source lock and
replays that accepted Act.

The stages therefore form one traceable pipeline:

```text
providers ───────────────→ Map IR ──→ projections ──→ Query / Path
                              │
                              └─────→ constraints ──→ Verification
                                         │
Map IR + Verification + intent ──────────┴──→ Plan (derived or authored)
                                                   │
source + executor input ───────────────────────────┴──→ Act candidate
                                                            │
                                                fresh Map + Verify
                                                            │
                                                    accepted Act → Apply
```

Every output retains its source and configuration links. Missing, conflicting,
incomplete, and stale information remains visible instead of being guessed
away.

## Command-line workflow

The CLI follows the product stages directly. Run it from the repository root;
`archbird.json` is discovered automatically when present.

### Map a repository

```bash
npx archbird map .
npx archbird map . --view architecture \
  --group-by component --level file --relations imports,calls
npx archbird map . --view tests --group-by directory
npx archbird map . --view evidence --detail full
```

Map scans the configured or discovered scope and builds the reusable repository
model. The default Markdown is an architecture-first overview. Canonical JSON
is exhaustive; views, grouping, relation filters, and detail only change its
presentation.

### Query focused context

```bash
npx archbird query . --symbol 'src/runtime.c:runtime_start' \
  --depth 1 --test-depth 1 --max-chars 12000 --check
npx archbird query . --search 'provider registry' \
  --search-limit 8 --max-chars 12000 --check
npx archbird impact . --path src/runtime.c --depth 2 --check
npx archbird query . --symbol 'src/runtime.c:runtime_start' \
  --view source --detail standard --max-chars 12000 --check
npx archbird query . --path src/runtime.c --dump --check
```

Query selects and ranks a task-sized neighborhood from the Map. Search supplies
advisory lexical seeds when an exact identity is unknown; it is not semantic or
natural-language search. Switch to a typed selector once the target is known.

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

### Find a connection with Path

```bash
npx archbird path 'src/cli.c' 'src/runtime.c' --root . \
  --relation calls --direction downstream --check
```

`path SOURCE TARGET` searches the Map's exhaustive typed graph for bounded
shortest witnesses. It preserves relation kind, direction, evidence state,
semantic resolution, provenance, and completeness. `found` requires a current,
source-evidenced route; candidate-only connectivity remains `unknown` and
fails `--check`.

### Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The project configuration described
below therefore needs no second suite file.

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

`provider_surface` normally treats configured providers as one combined
declaration surface. Set `require_all_providers: true` when every capability
must appear in every configured provider. Verify then reports the exact
missing provider witnesses.

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

#### Add observed test evidence

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

### Plan, Act, and Apply

`plan` evaluates the complete current policy and produces one editable
language-neutral artifact. `act` grounds its operators through native language
executors, rebuilds the isolated after-state, and emits only an accepted Act
without writing. `apply` revalidates and replays that Act without reevaluating
the Plan or rerunning an executor.

An exact missing symbol or test route whose code is not derivable remains a
structured non-executable `add_symbol` or `add_test_route` operation. Plan can
order implementation before declaration and tests; Act still refuses the Plan
until a reviewed executor or agent supplies the unresolved semantics.
Supply reviewed full-file content for one exact `add_symbol` destination, or
for an `add_test_route` item with one exact mapped test file, with repeatable
`act --submit ITEM=FILE`. The symbol destination may already exist or may be
absent. This is Act executor input, not a Plan rewrite: native Act observes the
destination, emits an exact create or replace transition, records the executor
ledger, builds one isolated after-Map, and accepts only when fresh Verify
closes every item constraint and preserves the rest of the policy.
`plan --format markdown` renders the canonical Plan as a review packet.
Absent or ambiguous test locations remain manual.
An exact missing `required_file_edge` becomes a neutral `add_dependency`
objective over its source, target, relation, and optional name. Submit reviewed
source-file content with the same `--submit` interface; fresh Map and Verify
must observe the requested edge before Act is accepted. Its exhaustive
source-scoped `file_edges` delta permits only that one addition.
An exact current `forbidden_file_edges` violation between mapped files becomes
the symmetric neutral `remove_dependency` objective. Reviewed source bytes
must remove the relation from the fresh after-Map. Act rejects any unrelated
edge addition or removal even when Verify passes. Component edges, incomplete
evidence, and external or unmapped targets remain redirect/manual work.

```bash
npx archbird plan --format markdown
npx archbird plan --output .archbird/plan.json
npx archbird plan CORE-PUBLIC-API --output .archbird/plan.json
npx archbird plan CORE-PUBLIC-API --rename oldApi=newApi \
  --output .archbird/plan.json
npx archbird plan FFI-SURFACE --git-diff HEAD \
  --output .archbird/plan.json
npx archbird map --format json --output .archbird/before-map.json
# Or, after saving an explicit Map before a partial migration:
npx archbird plan FFI-SURFACE --before-map .archbird/before-map.json \
  --output .archbird/plan.json
npx archbird act .archbird/plan.json
npx archbird act .archbird/plan.json \
  --submit IMPLEMENT_ITEM=reviewed-module.js \
  --submit TEST_ITEM=reviewed-test.js
npx archbird act .archbird/plan.json --format patch
npx archbird act .archbird/plan.json --format json \
  --output .archbird/act.json
npx archbird apply .archbird/act.json
```

The neutral `redirect_dependency` operator stores an exhaustive edge
ProjectionPlan, the relation and symbol identities, and affected source paths.
It stores no source ranges or replacement text. Act reevaluates the projection
once and dispatches its typed evidence by mapped source language. The native C
executor resolves exact declaration, definition, include, and call evidence.
The native Python executor requires one exact imported binding, exact
CPython-AST call evidence, and an already observed import-module spelling for
the replacement; it preserves explicit local aliases. The native ECMAScript
executor supports JavaScript, TypeScript, and TSX named imports when
Tree-sitter proves each binding and this frontend's TypeScript provider proves
every redirected call. It uses a replacement module spelling already observed
from the same source directory and preserves explicit aliases, including
self-aliases. Incomplete evidence, ambiguous definitions, missing observed
include/import routes, multi-name imports, non-call references, and non-unique
calls block execution.

Older exact `replace_range`, `delete_file`, `move_file`,
`edit_json_pointer`, and source-bound file deletion operations remain
available for bounded developer- or agent-authored edits. They are not the
model for derived Plan operators. An exact `required_paths` issue becomes a
path-only, input-required `create_file` objective; pass reviewed bytes through
`act --submit ITEM=FILE`, not through Plan. `add_dependency` and
`remove_dependency` follow the same input-required boundary for one exact
existing source file. An asserted
`edit_json_pointer` operation changes one reviewed
manifest/export-table value under an exact source hash, RFC 6901 pointer, and
expected old JSON value without reformatting the complete file. A derived
one-extra/one-missing rename candidate is review evidence, not intent; it
remains non-executable until `--rename OLD=NEW` is supplied. The reviewed Plan
stores only the language-independent symbol objective, exhaustive
`symbol_occurrences` ProjectionPlan identity, and repository-relative source
scope. Act reevaluates the same complete evidence. Tree-sitter establishes
ECMAScript declarations and import/export bindings; the TypeScript compiler
must establish reference targets while preserving aliased local names.
Qualified identities may rename only their terminal identifier while
preserving the same enclosing identity. Unrelated same-name declarations
outside the selected projection scope are not renamed. Public alias and
CommonJS assignment targets are separate reviewed surface identities, not
internal declaration occurrences. Candidate or unresolved calls, duplicate
targets, and unsupported inputs block Act instead of producing a partial
rename.

For a `provider_surface` issue, Plan can add a uniquely missing Make
registration, C header declaration, or bounded C/N-API export registration,
or use a reviewed rename to replace one stale Make registration with a
uniquely resolved surface member. Plan emits a neutral provider objective and
configured provider identity. Act validates the current configuration and Map.
Act/Make derives the source spelling and requires one direct token match.
Act/C requires one effective mapped C file, one mapped
`napi_<capability>` wrapper, syntax-clean source, and a mapped
`DECLARE_NAPI_METHOD` or descriptor peer. Multi-file, ambiguous, duplicate,
unresolved, or structurally unsupported provider cases remain manual or block
Act.
If the replacement capability is already registered, Plan emits removal of
the stale old capability rather than a duplicate registration.

Supplying `--before-map` allows the native compiler to finish one exact
observed provider-surface rename without a separate `--rename`. The old member
must have resolved uniquely before; exactly one new current member must retain
the same implementation paths and use ledger; and both declaration and
implementation signatures must differ only at the identifier. Both Maps must
share project, configuration, and producer identities. Incompatible,
diagnostic-bearing, signature-poor, or ambiguous histories remain
non-executable.

`--git-diff REVISION` builds the before Map from one Git commit through an
isolated raw-object snapshot and the ordinary discovery/provider pipeline. It
uses the current project configuration, does not run checkout filters or
mutate the source worktree, removes the temporary snapshot after mapping, and
reuses content-addressed provider facts. Revision ranges, saved-current-Map
mode, and simultaneous `--before-map` are rejected.

A `required_symbols` constraint naming one exact C header can derive
the language-neutral `declare_symbol` objective from one unique implementation
that establishes a bounded same-language evidence scope. Plan stores only the
symbol, destination, and exact source paths the executor may read. Act/C then
requires a declaration/definition peer, rederives the exact single-line
implementation source and placement, and rejects source-closure drift, globs,
ambiguous or internal implementations, and unproven header decoration. The
declaration and a derived Make registration can be accepted and applied
together.

If the constraint itself requires an implemented and used surface member that
is not registered, Plan can derive `add_provider_capability` without an extra
flag for each supported exact provider. Act/Make derives the direct or
leading-underscore convention from current source. Act/C can clone one mapped
C-header or N-API peer. Each executor selects one unique editable anchor by
canonical-name locality. The item is `derived`; incomplete, ambiguous,
duplicate, multi-file, or anchorless evidence stays manual or blocks Act.

Several missing members in the same Make provider remain separate Plan
obligations but materialize as one source-locked file transition. Only
distinct insertions sharing the same variable, anchor, and side compose;
tokens are ordered canonically. Byte-identical edits from separate Plan
obligations compose once and retain every item ID; incompatible overlaps
remain conflicts.

Plan can also derive removal of one unresolved, unused Make registration when
the Map proves zero candidates, zero uses, one exact declaration, and another
uniquely resolved declaration from the same provider. It will not empty a
configured provider, treat two stale entries as proof for each other, or turn
a mixed replacement into an inferred deletion.

The Plan operations are `add_provider_capability`,
`remove_provider_capability`, and `rename_provider_capability`; they contain
no Make token, anchor, byte range, replacement text, or source hash. The
native Act/Make and Act/C executors own those details and preserve assignments,
comments, continuations, whitespace, line endings, and unrelated source.
`--format patch` renders the accepted Act as a unified diff; it does not
create another artifact.

An exact missing npm package entrypoint becomes a neutral
`set_package_entrypoint` Plan operation when one package and a literal
package-relative target are proven. Native Act grounds direct `main`,
`exports`, and existing-object `bin` routes through the lossless JSON editor.
The target must be an existing regular file but need not be mapped, so
extensionless npm executables work. Its exact state is sealed as a read-only
Act source lock. Conditional or nested exports, ambiguous packages, and
missing targets remain manual.

Existing transition sources and every read-only executor input use SHA-256
locks; ranges use UTF-8 byte offsets and include expected text. Manual items
expose missing transformation inputs and block Act instead of inventing code.
Act evaluates the complete prepared file set
through `Project.withSourceOverlay()`, deriving a fresh Map and every
source-policy constraint before the first write. Incomplete relation evidence
blocks destructive generation. Failed, unknown, or unsatisfied fresh
acceptance writes nothing; only a satisfied after-state emits an accepted
Act. Apply then advances through source-lock revalidation and transactional
replay. Plan input is bounded to
64 MiB, collections and touched files to 4,096, individual source files and
patches to 64 MiB, and aggregate touched source and patch output to 256 MiB.
Project compilers and tests remain external; their reviewed observations can
participate in Verify.

### Explore the live repository

Run the local application while source changes:



```bash
npx archbird serve
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.
Live Map, projection, Query, Verify, and source work runs in the native Node
host; the page receives typed ProjectionResults and does not load browser Wasm.
Normal exploration does not download the canonical Map; saving it is explicit.

### Reuse saved evidence safely

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

npx archbird path 'src/cli.c' 'src/runtime.c' \
  --map .archbird/map.json --relation calls --direction downstream --check

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

Unchecked saved-Map queries accept supported older producers. Add `--check`
when the result will drive a decision; the shared core then requires the saved
producer digest to match the active core. Run `freshness` before treating the
saved source/config evidence as current:

```bash
npx archbird freshness --root . --snapshot .archbird/map.json --check
```


## JavaScript APIs

### Node

```js
const {
  Project,
  auditMapFreshness,
  compilePlan,
  renderPlanMarkdown,
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
    console.log(renderPlanMarkdown(planJson).toString("utf8"));
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
`renderPlanMarkdown()` presents the same validated Plan as a concise task
packet; it does not create or modify an artifact.
`materializeAct()` produces exact binary-safe transitions from a Plan. Its
optional `executorSubmissionsJson` option supplies reviewed full-file content
for exact unresolved symbol, test-route, required-path, or dependency items;
the corresponding `planSourceRequirements()` option asks the host for the
exact state required by each objective. Native Act then chooses create or
replace where the objective permits either. Submissions are ephemeral executor
input and never mutate Plan.
`acceptAct()` seals them only after callers supply the fresh isolated
after-Map and Verification. `preflightActApply()` returns `ready` or
`already_satisfied` after comparing newly observed sources with the complete
sealed before/after states; partial application and unrelated drift fail. The
explicit filesystem helpers `observePlanSources()`,
`actOverlay()`, `runActGates()`, `renderAct()`, and `applyAcceptedAct()`
provide that host transport. Reviewed `gates` from `archbird.json` execute as
direct argument arrays over the copied after-state; the native core rejects
incomplete or non-passing result ledgers before acceptance. All Plan
interpretation, edit materialization, gate-result validation, and acceptance
remain in the native core.

<!-- archbird-node-api:start -->
| Area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map, Query, and Path | `analyzeWorkspace`, `auditMapFreshness`, `diffMaps`, `exportGraph`, `pathMap`, `pathMapMarkdown`, `queryMap`, `queryMapMarkdown`, `renderMapMarkdown`, `renderPathMarkdown`, `renderSourceMarkdown`, `resolveDiscovery` |
| Projection and policy | `compileProjectConfiguration`, `compileQueryPlan`, `evaluateConstraints`, `evaluateProjection`, `freezeConstraints`, `reportConstraints` |
| Plan and Act | `acceptAct`, `actOverlay`, `actSourceRequirements`, `applyAcceptedAct`, `compilePlan`, `materializeAct`, `observeActSources`, `observePlanSources`, `planSourceRequirements`, `preflightActApply`, `renderAct`, `renderPlanMarkdown`, `runActGates`, `validateAct`, `validatePlan` |
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

## Agent workflow

Copy this compact policy into a project's `AGENTS.md`, `CLAUDE.md`, or
equivalent agent instructions:

```text
Use Archbird before broad source exploration.

- Start with `npx archbird map . --view overview --detail standard --max-chars
  12000 --check`.
- When an exact identity is known, use `npx archbird query . --symbol
  'PATH:SYMBOL' --depth 1 --test-depth 1 --max-chars 12000 --check`.
- When the identity is unknown, use `npx archbird query . --search 'CONCISE
  REPOSITORY TERMS' --max-chars 12000 --check`. Search is lexical and advisory.
- Read an exact declaration with `npx archbird query . --symbol 'PATH:SYMBOL'
  --view source --detail standard --max-chars 12000 --check`. Read one complete
  file with `npx archbird query . --path PATH --dump --check`; do not combine
  `--dump` with `--max-chars`.
- Use `npx archbird path SOURCE TARGET --check` for explicit connection
  questions. Candidate-only or incomplete connectivity remains `unknown`.
- Run `npx archbird verify --root . --check` before and after
  architecture-sensitive work. Treat static test routes as navigation, not
  proof of execution.
- Check `npx archbird freshness --root . --snapshot .archbird/map.json
  --check` before reusing a saved Map.
- Review generated Plans and accepted Acts. Never run `npx archbird apply`
  unless repository mutation is explicitly authorized and the exact Act was
  reviewed.
- If Archbird disagrees with source, inspect source directly and report a
  general reproducer; never hide uncertainty or manufacture observed evidence.
```

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
| `gates` | reviewed build or test commands that must pass in Act's isolated after-state |
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

Map uses repository-local C/C++ include search paths from compilation
databases in compiler order. Each translation unit's context follows its
literal includes through reached headers, and variants must agree on one
selected target. Resolved edges cite only the repository-relative database
path; external roots and absolute machine paths remain private.

The embedded config is mirrored by `examples/minimal.archbird.json`; the
complete multi-language form is `examples/quickstart.archbird.json` in the
source distribution.

The npm package exports the versioned JSON schemas for offline editors and
agents. For example,
`require.resolve("archbird/schema/archbird.schema.json")` locates the exact
project-configuration schema shipped with the installed engine. The native
configuration compiler remains authoritative for relational invariants that
standard JSON Schema cannot express.

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
input evidence provider. Verification results can render SARIF or JUnit. Plan
and Act remain canonical JSON artifacts; Plan additionally has a native
Markdown task-packet view.

<!-- archbird-node-cli:start -->
The CLI command names are `map`, `config`, `query`, `impact`, `path`, `diff`,
`observe`, `freshness`, `workspace`, `verify`, `plan`, `act`, `apply`, `export`,
`serve`, and `support`.
<!-- archbird-node-cli:end -->

`config` provides `show|init`; `export` provides
`json|graphml|mermaid`. Use
`npx archbird COMMAND --help` for flags. Exit status is 0 for success, 1 when
requested `--check` blocks, and 2 for invalid input or configuration.

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
