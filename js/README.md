# Archbird for JavaScript

**Map codebases. Verify architecture. Plan and check structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

```bash
npm install --save-dev archbird

npx archbird .          # map the current repository
npx archbird serve .    # explore it in the local web application
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
  console.log(overview.toString("utf8"));
  console.log(context.toString("utf8"));
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

npx archbird impact --map .archbird/map.json \
  --path src/runtime.c --depth 2
```

Unchecked saved-Map queries accept supported older producers. Add `--check`
when the result will drive a decision; the shared core then requires the saved
producer digest to match the active core. Use `freshness` separately to compare
the saved source/config evidence with a newly derived live Map.

The default is an architecture-first overview. Canonical JSON contains every
selected file and mapped fact; Markdown is only a human projection:

```bash
npx archbird . --view overview --detail compact
npx archbird . --view architecture
npx archbird . --view audit --detail standard
npx archbird . --view audit --full
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
npx archbird serve . --config archbird.json
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
`src/**/*.c` is recursive. Components group selected files; they do not discover
new files. `route_to` is broad asserted intent; `case_routes` is case-specific.
Patterns use the pinned `archbird-pcre2-v1` contract rather than JavaScript
`RegExp`.

The embedded config is mirrored by `examples/minimal.archbird.json`; the
complete multi-language form is `examples/quickstart.archbird.json` in the
source distribution.

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
npx archbird verify --config architecture.verify.json --check
npx archbird verify --config architecture.verify.json \
  --format sarif --output .archbird/architecture.sarif --check
```

Verify supports set/value equality, mapped names/values, directional subsets,
cardinality, required/forbidden/allowed edges, acyclicity, minimum test routes,
and behavioral-attestation equality. Facts can come from symbols, values,
component/file edges, provider surfaces, test routes, Python/C structured
entries, and supplied attestations.

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
# Before-state containing the finding.
npx archbird verify --config architecture.verify.json --format json \
  --output .archbird/before.verify.json

# Derived proposal: postconditions, candidates, coverage, and unknown frontier.
npx archbird plan --verification .archbird/before.verify.json \
  --finding FINGERPRINT --output .archbird/change.proposal.json

# Asserted human review; the proposal remains immutable.
npx archbird contract --proposal .archbird/change.proposal.json \
  --objective "Restore the reviewed public surface" \
  --owner core --rationale "Reviewed evidence and implementation strategy" \
  --preserve-all --output .archbird/change.contract.json

# An external person, agent, IDE, or codemod edits and tests the repository.
npx archbird verify --config architecture.verify.json --format json \
  --output .archbird/after.verify.json

# Derived transition judgment.
npx archbird verify-plan \
  --proposal .archbird/change.proposal.json \
  --contract .archbird/change.contract.json \
  --before-verification .archbird/before.verify.json \
  --after-verification .archbird/after.verify.json --check
```

Results distinguish satisfied, missing, unexpected, unknown, stale, and
superseded. Candidate paths are cited advice, not write authorization. Relevant
evidence changes invalidate a proposal; unrelated changes are context drift.

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
