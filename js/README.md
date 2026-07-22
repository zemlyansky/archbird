# Archbird for JavaScript

**Map codebases. Verify architecture. Plan and check structural changes.**

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
npx archbird serve      # explore it in the local web application
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

`npx archbird` and `npx archbird .` remain supported shortcuts for mapping the
current repository. The explicit `npx archbird map` form is useful in scripts
and alongside the other stage commands.

## Use from JavaScript

### Node API

```js
const { Project } = require("archbird");

const project = Project.fromRepository(".");
try {
  const mapJson = project.mapJson({ pretty: true });       // canonical bytes
  const overview = project.mapMarkdown({
    view: "overview",
    detail: "standard",
    maxChars: 12000,
  });
  const context = project.queryMarkdown({
    symbols: ["src/runtime.c:runtime_start"],
    depth: 1,
    context: { profile: "change" },
    maxChars: 8000,
  });
  const brief = project.queryMarkdown({
    symbols: ["src/runtime.c:runtime_start"],
    depth: 1,
    view: "changes",
    detail: "compact",
  });
  console.log(overview.toString("utf8"));
  console.log(context.toString("utf8"));
  console.log(brief.toString("utf8"));
} finally {
  project.dispose();
}
```

`Project.fromRepository()` applies discovery, repository configuration, and
explicit options. `Project.fromConfig()` uses exactly one reviewed config.
Canonical JSON methods return stable artifact bytes or decoded objects;
Markdown and graph views are derived presentations and are not verification
inputs.

Saved-Map functions `queryMap()` and `queryMapMarkdown()` accept
`producerPolicy: "compatible"` (default) or `"current"`. The current policy
rejects a missing or different core producer digest; it is independent of the
live-source comparison performed by `auditMapFreshness()`.
`queryMapMarkdown(map, { view: "changes", verificationResult: resultJson })`
adds only constraints with exact subject-side source-path overlap and reports input
and producer freshness. The browser `Project.queryMarkdown()` accepts the same
option.

Pass `{ search: ["provider registration"], searchLimit: 8 }` when the path or
symbol is not yet known. Archbird returns advisory candidate seeds with the
exact matched Map fields and scores, then expands them through normal typed
Query routes. This deterministic lexical ranking does not claim semantic
equivalence, and its transitive symbol neighbors do not strengthen static test
routes.

Test matches report file distance and symbol-hop distance independently. A
case-local call to a traversed wrapper can therefore rank above same-file
fallback without being presented as runtime coverage.

### Import observed test routes

Run each case in isolation with V8 or Istanbul coverage, then convert the
project-owned reports without rerunning the project:

```bash
npx archbird observe . --map .archbird/map.json \
  --request .archbird/coverage-request.json \
  --output .archbird/test-symbols.json
```

`compileTestObservations(map, request, { repository, requestDirectory })`
provides the same Node API. The Node host supports V8, Istanbul, LLVM, and gcov
JSON. One isolated report is required per case because aggregate reports cannot
establish which test produced a hit; use the PyPI host for coverage.py dynamic
contexts. Archbird reads and source-hash-checks the reports but never launches
the test runner.

### Browser API

```js
const { createBrowserArchbird } = require("archbird/browser");

const archbird = await createBrowserArchbird();
const source = new archbird.Source(
  "src/index.ts",
  new TextEncoder().encode("export function answer() { return 42; }\n"),
);
const project = archbird.Project.fromFiles([source]);
try {
  console.log(project.map());
} finally {
  project.dispose();
}
```

The direct browser API runs in the caller's context. The visualization app and
`archbird/worker` isolate analysis in a Worker. Browser Map and Query use the
same canonical formats as Node; include configured SCIP bytes among supplied
files when semantic index evidence is required.

## Command line

Save complete evidence once and query it without re-analyzing:

```bash
mkdir -p .archbird
npx archbird map . --format json --pretty \
  --output .archbird/map.json --check

npx archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000

npx archbird query --map .archbird/map.json \
  --search 'where is provider registration handled' --search-limit 8

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
npx archbird map --view architecture
npx archbird map --view audit --detail standard
npx archbird map --view audit --full
```

`--compact` and `--full` alias the corresponding detail levels. Query context
uses the `exact`, `change`, `architecture`, and `audit` profiles plus per-kind
quotas, route provenance/confidence, candidate/conservative policy, and finally
`--max-chars` as a guard. `--progress auto` updates one terminal line for long
interactive runs and stays silent when output is piped; use `always` or `never`
to override it.

`direct`, `candidate`, and `conservative` are static evidence strengths, not
claims that a test ran. Use project-runner observations for executed routes.

Run the local application while source changes:

```bash
npx archbird serve --config archbird.json
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.

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
  ],
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
<!-- archbird-minimal-project-config:end -->

Configuration can additionally declare:

| Section | Purpose |
| --- | --- |
| `packages`, `builds`, `artifacts` | manifests, public entrypoints, compilation-database/Autoconf/Make/npm routes, logical outputs and loaders |
| `bridges` | declared/used/implemented ABI, binding, or message surfaces |
| `tests` | static cases, reviewed `case_routes`, and generated-source relations |
| `named_entries`, `parity` | configured entrypoint protocols and reviewed surface relationships |
| `indexes` | one or more SCIP indexes with prefixes, position encoding, and build variants |
| `projections`, `queries`, `constraints` | reusable derivations, saved Query plans, and reviewed architecture policy |
| `limits` | bounded Map analysis policy |

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

## Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The quick-start configuration above
therefore needs no second suite file.

```bash
# Run one saved Query plan or an ad-hoc query.
npx archbird query public-api-impact
npx archbird query --symbol demo_open --direction upstream

# Evaluate the whole reviewed policy or one named constraint.
npx archbird verify --check
npx archbird verify CORE-PUBLIC-API --check

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
as whole-policy compliance. Unknown IDs are errors. Repository selection is
execution context: run in the project root or use `--root PATH`; an external
configuration uses `--config CONFIG --root PROJECT`.

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

## Act: review and judge a change

Archbird does not make the edit. It turns one failed architecture rule into a
proposed change checklist, records the checklist after review, and compares the
before and after repository state to see whether the required changes happened.

```bash
# Before-state containing the finding.
npx archbird verify --format json \
  --output .archbird/before.verification.json

# Derived proposal: postconditions, candidates, coverage, and unknown frontier.
npx archbird plan --verification .archbird/before.verification.json \
  --finding FINGERPRINT --format markdown \
  --output .archbird/change.task.md

npx archbird plan --verification .archbird/before.verification.json \
  --finding FINGERPRINT --output .archbird/change.proposal.json

# Asserted human review; the proposal remains immutable.
npx archbird contract --proposal .archbird/change.proposal.json \
  --objective "Restore the reviewed public surface" \
  --owner core --rationale "Reviewed evidence and implementation strategy" \
  --preserve-all --output .archbird/change.contract.json

# An external person, agent, IDE, or codemod edits and tests the repository.
npx archbird verify --format json \
  --output .archbird/after.verification.json

# Derived transition judgment.
npx archbird verify-plan \
  --proposal .archbird/change.proposal.json \
  --contract .archbird/change.contract.json \
  --before-verification .archbird/before.verification.json \
  --after-verification .archbird/after.verification.json --check
```

Results distinguish satisfied, missing, unexpected, unknown, stale, and
superseded. Candidate paths are cited advice, not write authorization. Relevant
evidence changes invalidate a proposal; unrelated changes are context drift.
Task Markdown shows 20 evidence rows by default; use `--full` for all rows.

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
| Vue | lexical | lexical |
| supplied SCIP | native semantic decoder | Wasm semantic decoder |

For CPython-AST evidence, use the PyPI host. Tree-sitter recovery is fact-local;
SCIP retains producer, document coverage, source anchoring, and freshness.
Provider conflicts, ambiguity, and unresolved targets remain explicit.

Node's per-file provider facts and materialized complete unchanged Maps are
content-addressed and revalidated against the native/Wasm core, configuration,
selected source bytes, and provider implementations. Both tiers share a 1 GiB
budget and evict the oldest entries;
`--cache-max-bytes` or `ARCHBIRD_CACHE_MAX_BYTES` changes it, `--cache-dir`
selects the root, and `--no-cache` disables it. Failed temporaries are removed
on the next use, and a full cache warns without changing canonical output.

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
input evidence provider. Verify and change results can render SARIF or JUnit.

The CLI commands are `map`, `config show|init`, `query`, `impact`, `freshness`,
`diff`, `workspace`, `verify`, `plan`, `contract`, `verify-plan`,
`export json|graphml|mermaid`, `serve`, and `support`. Use
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

Requires Node 18+. Archbird is Apache-2.0 licensed. Full strict schemas and the
C/Python hosts are included in the source repository; this README is the
complete npm/Node/browser usage contract.
