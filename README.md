# archbird

**Map codebases. Verify architecture. Plan and apply structural changes.**

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
archbird plan             # derive a Plan from current constraint issues
archbird act PLAN.json    # ground and verify an Act; writes nothing
archbird apply ACT.json   # replay the accepted Act
archbird serve          # npm: npx archbird serve
archbird mcp            # Python/root launcher: local agent protocol
```

## Language support

All interfaces produce the same Map model. The C core provides portable lexical
and Tree-sitter syntax analysis; the Python, Node, and browser hosts add the
precision available in their own runtimes.

| | libarchbird | archbird.js | archbird.py | archbird app |
| --- | --- | --- | --- | --- |
| C and C++ | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical |
| Python | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical | ✓ CPython AST + Tree-sitter + lexical | ✓ Tree-sitter + lexical |
| JavaScript, TypeScript, and TSX | ✓ Tree-sitter + lexical | ✓ TypeScript compiler + Tree-sitter + lexical | ✓ Tree-sitter + lexical | ✓ TypeScript compiler + Tree-sitter + lexical |
| R | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical | ✓ Tree-sitter + lexical |

Lexical providers conservatively recover declarations, calls, and explicit
protocols from source text. Tree-sitter adds syntax, scopes, imports, and exact
spans. CPython AST and the TypeScript compiler add host-specific precision.
SCIP is optional input for resolved definitions, references, and relationships;
Archbird reads a supplied index but does not invoke an indexer.

## Stages

| Stage | Question | Output |
| --- | --- | --- |
| **Map** | What exists, and how is it connected? | Searchable files, symbols, dependencies, tests, and build routes |
| **Query** | Which exact evidence matters for this task? | Focused, ranked context with source witnesses |
| **Verify** | Does the code follow the architecture constraints? | Constraint status, violations, code locations, and unknowns |
| **Plan** | What structural change follows from current evidence, and what remains unknown? | An editable language-neutral Plan with objectives, operators, applicability, and acceptance constraints |
| **Act** | How does that Plan ground into exact repository changes, and does its after-state pass? | An accepted, sealed Act bound to exact transitions plus fresh Map and Verification evidence |

**Map handles fragmentation.** In a complex repository, architecture is spread
across source languages, package manifests, public interfaces, native/frontend
bridges, tests, build systems, and generated artifacts. Map joins those parts
into one navigable model, so a developer or coding agent can locate a symbol,
follow its dependencies and consumers, and see which tests and delivery
surfaces are connected to it.

**Verify handles architectural drift.** Map describes what the repository
currently contains; constraints describe what must remain true. Verify checks
the complete relevant Map rather than a shortened query view and reports which
constraints pass, which fail, where the violation occurs, and where incomplete
or stale information prevents a reliable answer.

**Plan and Act handle coordinated change.** A structural fix may need
synchronized updates to an implementation, public interface, language binding,
package entrypoint, tests, and build artifacts. Plan derives evidence-bound
operators only where current Map and Verify evidence establish sufficient
applicability. Underdetermined work remains a visible manual item rather than
guessed code. A developer or agent may edit the Plan. Act selects the
language-specific executor, grounds the operator into exact transitions,
evaluates those bytes through an isolated source overlay and fresh
Map/Verification, and writes no repository files. Apply revalidates source
locks and replays only the already accepted Act; it never reevaluates the Plan
or reruns an executor. `archbird act --format patch` renders the same Act as a
unified patch for review; patch is not a separate artifact.

Across all stages, results retain links to the source, configuration, or
test data that produced them. Archbird keeps ambiguous, incomplete, and stale
information visible instead of guessing. Repository mutation occurs only
through `archbird apply ACT.json`.

## Map

Start in any repository:

```bash
cd project
archbird map .
archbird query . --symbol runtime_start
archbird query . --search 'provider registry'
archbird serve --root .
```

`map` scans the configured/discovered scope and builds Archbird's canonical
derived evidence model of files, symbols, dependencies, public interfaces,
tests, build routes, artifacts, and components. Unsupported inputs, unresolved
relationships, provider recovery, and other unknown frontiers remain explicit.
The default Markdown output is a readable architecture overview; use JSON when
you want to save the canonical Map for later commands.

`query` loads or builds that Map, selects a starting point, follows its recorded
relationships, and returns a focused neighborhood. Use typed selectors such as
`--symbol`, `--path`, `--component`, `--package`, or `--artifact` when you know
what you are looking for.

`query --search` helps find that starting point when you do not know its exact
path or symbol. It ranks lexical matches from repository names, paths,
signatures, descriptions, and package metadata, then runs the same focused
Query from those candidates. It tolerates prefixes, substrings, and small
typos, but it does not interpret a natural-language question or turn a text
match into a proven code relationship.

`serve` starts a browser-based architecture explorer and immediately prints its
loopback URL. Use it to expand selected components into files and selected files
into symbols while the rest of the architecture stays collapsed; inspect typed
connections and source witnesses; overlay configured constraint coverage and
findings; run focused queries; and compare Map snapshots as the repository
changes. Analysis progress is visible from the first page render. If an update
fails, the explorer reports the failure and keeps showing the last valid Map
and generation-matched Verification.

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
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000 --check

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

Query selectors accept exact or partial symbols, paths, mapped directories,
globs, layers, components, tests, packages, artifacts, builds, provider
surfaces, parity surfaces, and named entries. `query` is bidirectional by
default; `impact` starts upstream. Occurrence-backed symbol relations are used
before conservative file expansion.

`--search KEYWORDS` is deterministic lexical retrieval for cases where you do
not yet know a path or symbol. Use concise repository vocabulary such as
`provider registry`; it does not interpret questions, intent, or synonyms. It
ranks candidate symbols, files, components, packages, and artifacts from names,
paths, signatures, component descriptions, and package metadata, shows the
exact field and match behind every score, then expands the selected candidates
through the same typed graph. Prefix, substring, and bounded typo matches are
advisory seeds; they never become semantic edges or make a constraint pass.
Prefer `--symbol`, `--path`, or another typed selector once you know the target.
Symbol neighbors reached only from advisory seeds do not strengthen a static
test route.

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
archbird map --view overview --detail compact
archbird map --view architecture \
  --group-by component --level file --relations imports,calls
archbird map --view tests --group-by directory
archbird map --view evidence --detail full
archbird query --symbol runtime_start --view source
archbird query --symbol runtime_start --dump
```

`--view` selects a question-oriented preset. `overview` includes project
landmarks and broad package/build/test connections; `architecture` emphasizes
code dependencies; `tests` isolates test routes; and `evidence` exposes
coverage, diagnostics, and completeness. `--group-by` reorganizes the same
selected entities by directory, configured component, layer, or language.
`--level` chooses component, file, or symbol nodes. `--relations` overrides the
preset's relation families and accepts comma-separated values or repeated
flags. These semantic axes compile to one exhaustive graph ProjectionPlan and
the resulting typed ProjectionResult drives both Markdown and the application.

`--detail` changes rendering density only. `--compact` and `--full` are aliases
for its corresponding values. `--max-chars` is a final presentation guard; it
never changes the canonical Map or turns incomplete evidence into success.
Compact and standard Markdown rank structural groups, aggregated dependency
flows, and file landmarks while reporting what the presentation omitted.
`--detail full` enumerates the exhaustive selected graph records. Dependency
flow is shown provider to consumer; the underlying canonical relation remains
consumer to provider (`A` uses `B`).

Graph completeness and repository coverage are reported separately. A graph can
exhaustively represent every selected supported fact while discovery still
reports unsupported, ignored, oversized, or otherwise unknown repository
inputs. Presentation omissions never change either classification.
Query context profiles (`exact`, `change`, `architecture`, `audit`), per-kind
quotas, route provenance/confidence, and candidate/conservative policies remain
separate Query behavior.

`--view source` materializes source bytes selected by the Map or Query instead
of storing source text in the canonical artifact. Compact detail is an indexed
declaration outline. Standard detail expands exact declarations matched by a
symbol Query, returns a complete directly selected file for a path Query, and
leaves related files as outlines. Full detail returns every selected file;
`--dump` is an alias for `--view source --detail full`. Full source cannot be
combined with `--max-chars`, because silently dropping part of a requested dump
would make it misleading.

Every emitted file is matched by repository-relative path and SHA-256 against
the Map before its bytes are rendered. A saved Map therefore needs the source
checkout explicitly:

```bash
archbird query --map .archbird/map.json --root . \
  --symbol 'src/runtime.c:runtime_start' --view source
archbird query --map .archbird/map.json --root . \
  --symbol 'src/runtime.c:runtime_start' --dump \
  --output .archbird/runtime-start-source.md
```

When a provider cannot establish an exact declaration extent, standard source
shows the indexed outline and states that exact source is unavailable; it does
not guess where the declaration ends. Non-UTF-8 bytes and terminal control
sequences are hash-validated but are not embedded in Markdown.
When valid semantic-AST and concrete-syntax extents differ, the concrete
source boundary is canonical for rendering while every alternate boundary and
provider remains recorded in the merge-variation ledger.

Plain saved-Map queries accept every supported Map schema even when another
Archbird core produced the artifact. Add `--check` when the result will drive a
decision: it also requires the saved producer digest to match the active core.
That producer check does not establish live-source freshness; use `freshness`
for a new Map-to-repository comparison. C/API query requests use
`producer_policy: "compatible"|"current"`; every Query records the effective
policy and its `current`, `different`, or `unknown` producer classification.

Progress is adaptive: `--progress auto` updates one terminal line only when an
analysis takes long enough to notice and stays silent for pipes and agents.
Use `--progress always` for captured agent/CI logs or `--progress never` for
silence.

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
archbird serve --no-config
archbird serve --root ../project
```

`serve` prints a loopback URL immediately, analyzes in a worker, publishes only
valid generations, and retains the last good Map when a later candidate fails.
Live Map, Query, projection, Verify, and graph-view work runs in the server's
native Archbird core; the page does not fetch browser Wasm for live repository
exploration. Wasm is reserved for the static app's local folder/ZIP and saved
artifact workflows.
The application provides:

- useful zero-config exploration from the same exhaustive graph
  ProjectionResult used by Markdown: switch overview, architecture, tests, and
  evidence views, then independently group by directory, configured component,
  layer, or language; configured components and constraints refine the graph
  when `archbird.json` exists but never gate the zero-config path;
- one mixed-resolution architecture graph: double-click, press Enter, or use
  the inspector to expand a component or inferred layer through directories
  into files, then expand one file into symbols while unrelated groups remain
  collapsed; collapse, hide, and restore controls are presentation state and
  never rewrite Map or Projection evidence;
- entity-type colors, generation-matched constraint/finding overlays, keyboard
  navigation, pointer-centered normalized wheel zoom, slider zoom, layout
  direction, evidence-class, and edge-kind controls; selecting never changes
  the viewport, while expanding animates children from the activated node and
  keeps that node at the same screen position; dependency arrows default to
  provider → consumer flow and can switch explicitly to consumer → provider
  uses;
- compact node and edge hover details for identity, relation counts, evidence,
  and verification state; external symbol relations stay at the file frontier
  until explicitly revealed for one symbol;
- graph-local filtering plus focused typed or lexical Query from the current
  canonical Map;
- exact source witnesses for live repositories, retained last-good evidence,
  repository snapshots, and structural comparison between saved generations;
- canonical artifact and graph-view JSON downloads plus GraphML and Mermaid
  graph exports;
- task-oriented Diff and change-artifact summaries, Verification finding
  review with explicit waiver candidates, and project-configuration review;
- system, light, and dark themes with responsive graph and inspector layouts.

The same application can run statically on GitHub Pages. A browser can open a
saved artifact, local directory, or ZIP and analyze supplied files through the
Wasm Worker without uploading source or requiring a server. Directory input
passes path/size metadata and discovery documents through native discovery
before reading selected source bytes, so default-excluded dependency/build
trees do not fail merely because they contain large binaries. Saved artifacts
do not contain repository source bytes, so source viewing is available only
while a live server or browser repository session owns those bytes. Browser
directory and ZIP inputs are explicit snapshots rather than watched filesystem
handles; reload changed input to create another generation.
Server mode evaluates Map and ProjectionPlans in the native Node/Python host
and sends typed ProjectionResults to the page. Normal live exploration does not
download the canonical Map; `Save canonical artifact` fetches those bytes
explicitly. Static folder/ZIP and saved-Map workflows evaluate the same plans
in the browser Wasm host. A successfully
mapped repository with no supported source files shows an explicit empty-scope
state rather than a blank graph.

## Agent protocol

The Python CLI and source launcher expose the same live repository service over
MCP stdio:

```bash
archbird mcp
archbird mcp --root ../project
archbird mcp --no-config
```

An MCP host launches that command and communicates through stdin/stdout. The
server maps the repository once, watches it for changes, retains recent valid
generations, and exposes read-only tools for status, Map presentation,
exhaustive projections, focused Query, hash-checked source, Verify, and Diff.
It does not advertise Act because the current live service does not yet provide
Map-aware change planning.

Map, Projection, Query, source, Verification, and Diff results include
generation- and digest-bound resource links. Tool results are structured JSON
plus text where useful, and are bounded to 2 MiB; Map and Query have smaller
configurable presentation budgets. `archbird_source` validates the requested
repository-relative path and current bytes against the selected Map generation.
Project configuration can come from the repository or a file, but not stdin,
because MCP owns stdin.

The transport follows the
[MCP stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and the tool/resource shapes follow the
[MCP server specification](https://modelcontextprotocol.io/specification/2025-11-25/server).

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
and Wasm compilers aligned for their common contract. Project configuration is
unversioned: absent project-model fields inherit discovery, while explicitly
present fields replace the corresponding discovered value. Generated Map,
ProjectionResult, Query, Verification, and change artifacts each carry an
independent schema version and migration schedule; Archbird has no global
schema version.

## Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The complete
[`quickstart.archbird.json`](examples/quickstart.archbird.json) combines these
stages and needs no second suite file.

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

`provider_surface` normally treats configured providers as one combined
declaration surface. Set `require_all_providers: true` when every capability
must appear in every configured provider, such as a public header and a Wasm
export list. Verify then reports the exact missing provider witnesses instead
of accepting a declaration found only on another surface.

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

## Plan and Act

Plan evaluates the complete current Verify policy and derives one editable
artifact. Omit the constraint name to include every current issue, or scope
generation by existing constraint ID:

```bash
# Inspect the current task DAG without saving a second artifact.
archbird plan --format markdown
archbird plan --output .archbird/plan.json
archbird plan CORE-PUBLIC-API --output .archbird/plan.json
# Review a derived rename candidate, then assert the intended transformation.
archbird plan CORE-PUBLIC-API --rename old_api=new_api \
  --output .archbird/plan.json

# Derive residual work from a committed base without changing the worktree.
# ...developer or agent partially changes the implementation and consumers...
archbird plan FFI-SURFACE --git-diff HEAD \
  --output .archbird/plan.json

# Or save an explicit Map before work begins.
archbird map --format json --output .archbird/before-map.json
# ...developer or agent changes the implementation and consumers...
archbird plan FFI-SURFACE --before-map .archbird/before-map.json \
  --output .archbird/plan.json

# Ground, verify, and inspect the exact Act without writing.
archbird act .archbird/plan.json
# Supply reviewed implementation semantics for exact unresolved items.
archbird act .archbird/plan.json \
  --submit IMPLEMENT_ITEM=reviewed-module.py \
  --submit TEST_ITEM=reviewed-test.py
archbird act .archbird/plan.json --format patch
archbird act .archbird/plan.json --format json \
  --output .archbird/act.json

# Replay only the accepted Act after review.
archbird apply .archbird/act.json
```

Plan's target contract is language-neutral: it names architectural objectives,
operators, exhaustive applicability, dependencies, executor capability, and
acceptance without embedding source syntax or replacement bytes.
When a reviewed constraint identifies one exact missing symbol or test route
but does not determine implementation code, Plan retains that target as a
structured non-executable `add_symbol` or `add_test_route` operation instead
of reducing it to an opaque manual note. This lets Plan order a semantic
implementation before its declaration and its tests while continuing to state
that Act has no authorized source transformation. A developer or agent may
submit reviewed full-file content for one exact `add_symbol` destination, or
for an `add_test_route` item when Map identifies exactly one file in its
reviewed test group, with repeatable `act --submit ITEM=FILE`. The symbol
destination may be an existing mapped file or a missing exact path. The Plan
remains byte-identical and language-neutral; the native Act core observes the
destination, chooses an exact create or replace transition, records
read/write/match evidence, builds one real isolated after-Map, and rejects the
entire Act unless the original and preserved constraints pass. A missing
definition, declaration, and test route can therefore form one useful
agent/developer task DAG without Archbird inventing a signature, body, or
test. `plan --format markdown` renders that same canonical Plan as a review
packet; it does not create another artifact. Absent or ambiguous test
locations remain manual.
An exact missing path from `required_paths` similarly becomes a path-only
`create_file` objective. Plan does not embed the future file bytes. Supply
reviewed content with `act --submit ITEM=FILE`; native Act requires the
destination to remain absent, previews one exact creation, and accepts it only
when the isolated after-Map closes the constraint.
An exact missing `required_file_edge` similarly becomes an input-required
`add_dependency` objective containing only the source path, target path,
relation kind, and optional relation name. A reviewed source-file submission
must produce that edge in the fresh Map; an unchanged or unrelated edit is
rejected with zero worktree writes.
An exact current `forbidden_file_edges` violation between two mapped files
becomes the symmetric input-required `remove_dependency` objective. Plan
records the exact source, target, and relation without choosing replacement
semantics. Reviewed source content must remove the forbidden relation from the
fresh Map; preserving the edge rejects the complete Act with zero worktree
writes. Aggregated component edges, incomplete evidence, and external or
unmapped targets remain on the reviewed redirect/manual path.
`redirect_dependency` and `declare_symbol` currently follow this boundary end
to end. Given reviewed `--redirect OLD=NEW` intent, Plan stores the exhaustive
edge ProjectionPlan, exact relation, affected source paths, and symbol
identities. Act reevaluates that projection once and dispatches its typed
evidence by mapped source language. The native C executor resolves exact
declarations, definitions, include spelling, and call sites. The native Python
executor resolves one exact imported binding and its CPython-AST calls, uses an
already observed import-module spelling for the replacement, and preserves an
explicit local alias. The native ECMAScript executor supports JavaScript,
TypeScript, and TSX named imports when Tree-sitter proves the binding and the
TypeScript compiler proves every redirected call. It uses a replacement module
spelling already observed from the same source directory and preserves explicit
aliases, including aliases whose local name equals the old imported name. The
Node frontend supplies the required TypeScript evidence by default; a
syntax-only Map is rejected. Incomplete projections, ambiguous definitions,
missing observed include/import routes, multi-name imports, non-call
references, and non-unique call sites block Act instead of producing partial
edits.

Low-level asserted operations currently include `replace_range`,
`delete_file`, `move_file`, and `edit_json_pointer`. They are useful for
bounded edits supplied by a developer or agent, but are not the model for
derived Plan operations. `create_file`, `add_dependency`, and
`remove_dependency` are
language-neutral, input-required Plan objectives; exact content belongs to Act
submission.

A one-extra/one-missing symbol constraint may suggest a rename, but that does
not establish intent: the derived candidate stays non-executable until a
developer or agent supplies `--rename OLD=NEW`. The reviewed command evaluates
one exhaustive `symbol_occurrences` projection seeded by the constraint's
selected declaration paths. Plan stores only the language-independent symbol
objective, normalized ProjectionPlan identity, and repository-relative source
scope. Act reevaluates the projection, requires identical complete evidence,
and lets the native Python or ECMAScript executor validate and materialize
exact declaration, import, export, binding, and reference edits. Unrelated
same-name declarations outside the selected scope are not renamed.

For an exact `provider_surface` issue, Plan can add a missing configured Make
registration, C header `file_pattern` declaration, or bounded C/N-API
`exports` registration. It can also use reviewed `--rename OLD=NEW` intent to
replace a stale Make entry when the target resolves uniquely. Each provider
definition has a canonical digest in Map and Plan, so distinct definitions
over one path do not collapse into one identity. Plan records the neutral
provider objective and bounded source scope; it stores no Make token, C
signature, export-table syntax, anchor, source hash, byte range, or replacement
text. Act validates the provider digest against the current configuration and
Map. Act/Make derives the token spelling and requires one registration anchor.
Act/C rederives a single-line declaration from the exact implementation and an
existing declaration peer. For an `exports` addition it requires one effective
mapped C file, one mapped `napi_<capability>` wrapper, syntax-clean source,
and an existing mapped `DECLARE_NAPI_METHOD` or N-API descriptor peer; it
clones that peer while preserving multiline layout and line endings.
Selectors that map the provider to multiple files, active uses of stale
entries, unresolved targets, ambiguous implementations, missing anchors,
multiple token matches, and distinct provider definitions that would require
one coincident source edit remain manual or are rejected by Act.
If the replacement is already registered by that provider, Plan normalizes
the target state to removal of the stale old registration instead of
proposing a duplicate.

For an exact `required_package_entrypoint` failure with one npm package and
one mapped target, Plan emits the language-neutral `set_package_entrypoint`
objective. Act's native JSON executor grounds `main`, direct `exports`, and
existing-object `bin` routes through the lossless JSON Pointer editor, then
requires the fresh after-Map to close the original constraint. Conditional or
nested exports, ambiguous package selectors, non-mapped targets, and route
shapes that would require replacing an existing object remain manual. Plan
contains the package route and target, not JSON syntax, byte offsets, or
replacement text.

`--before-map OLD.json` can derive the same provider registration replacement
without asserted rename intent when an observed partial migration proves it.
The old surface member must have been uniquely resolved in the before Map; the
current old registration must be unused and unresolved; and exactly one new
current member must retain the same implementation paths and use ledger with
declaration and implementation signatures differing only by the symbol name.
Both Maps must share project, configuration, and producer identities and have
no error diagnostics. Plan and Act retain both Map identities. Missing
signatures, incompatible histories, or multiple matching targets do not
authorize an edit.

`--git-diff REVISION` constructs that before Map from one verified Git commit
in an isolated raw-object snapshot under Archbird's cache directory, using the
current project configuration and ordinary discovery/provider pipeline. It
does not run checkout filters and never checks out or writes the source
worktree. The option accepts a commit, not a revision range, and cannot be
combined with `--map` or `--before-map`. The temporary snapshot is removed
after its canonical Map is built; unchanged provider facts remain eligible for
the normal content-addressed cache.

When a constraint itself requires a missing surface member, that reviewed
policy supplies the intent. If the member has one implementation candidate
and is already declared by another provider or has current uses, Plan derives
one `add_provider_capability` item per missing supported provider without
another flag. An exact C/N-API export may instead use the current bridge use
plus the uniquely mapped wrapper in its configured provider file. A C
declaration item precedes its Make or export registration for the same surface
member. Both the `missing` and `unregistered` findings remain origins, and
every generated item is explicitly `derived`. Incomplete evidence, ambiguous
implementations, unsupported provider kinds, or colliding edit targets remain
manual.

When several reviewed surface members are missing from the same Make
provider, Plan emits one item per obligation and Act composes their distinct
insertions into one source-locked file transition. Composition is limited to
the same variable, anchor, and side; tokens are ordered canonically.
Byte-identical edits produced by separate Plan obligations also compose once
while the transition retains every originating item ID. Coincident edits with
different effects remain conflicts.

For a `required_symbols` constraint scoped to one exact C header, Plan can
derive the neutral `declare_symbol` objective when one same-language mapped
definition establishes a bounded two-file evidence scope. Plan records only
the destination, symbol, and source paths that the executor may read. Act/C
then requires an existing declaration/definition peer and rederives the exact
single-line implementation signature and placement from those hash-checked
sources; no C syntax, byte range, source hash, or anchor is stored in Plan. It
preserves indentation and line endings. Globs, multiple implementations,
missing peers, multiline or internal definitions, header-only decoration
differences, and source comments at the insertion anchor remain manual or are
rejected. This can be combined with a missing Make registration in one Plan
and accepted as one two-file architecture change.

An unresolved, unused registration can likewise become a derived removal when
the Map proves zero implementation candidates, zero uses, one exact Make
declaration, and at least one other uniquely resolved declaration from the
same provider. The last guard prevents Plan from emptying a configured
provider or treating two stale entries as proof for each other. Mixed
replacement cases, active or ambiguous entries, duplicate token spellings,
and removals that do not close the complete constraint remain manual.

An asserted `edit_json_pointer` operation handles reviewed manifest and export
table changes without replacing or reformatting the whole file. It names one
source-locked file, one RFC 6901 pointer, the exact expected old JSON value (or
explicit absence), and the replacement value. The native core rejects stale
hashes, duplicate keys, missing parents, ambiguous expectations, and invalid
JSON, then returns one exact byte edit. Archbird does not derive this
representation-level intent from a generic architecture finding.

The native provider executors ground `add_provider_capability`,
`remove_provider_capability`, and `rename_provider_capability`. Act/Make
preserves assignment operators, comments, continuations, whitespace, and
unrelated variables. It handles direct tokens only; variable expansion is not
inferred. Act/C supports the bounded header and N-API forms above. Zero,
duplicate, ambiguous, stale, multi-file, or syntax-recovered matches block the
Act. Unified-diff text is selected with
`archbird act PLAN --format patch`; it is only an Act rendering, not another
artifact or lifecycle stage.

The CPython provider establishes Python declaration, binding, and reference
sites. Tree-sitter establishes ECMAScript declarations and import/export
bindings; the TypeScript compiler must establish ECMAScript reference targets,
including aliased import origins without rewriting their local aliases.
Candidate or unresolved calls, duplicate targets, unsupported inputs, and
lexical-only C/C++ call binding prevent Act from producing a partial edit.

Existing sources are locked by SHA-256; ranges use UTF-8 byte offsets and
include the expected text. Multiple range edits to one file become one atomic
file transition and one unified diff. Paths must be canonical
repository-relative paths. Symlinks, non-regular files, overlapping edits,
stale hashes, conflicting destinations, and dependency cycles block before the
first write.

Verify evidence often establishes a required state without establishing the
code that should implement it. Plan records those cases as non-executable
`manual` items with candidate paths and explicit unknowns. Act refuses to apply
until every item has a reviewed executable operation; it never invents a
function body, replacement dependency, package target, or test.

For file and component dependency issues, the exhaustive edge projection also
retains each exact inducing import, include, call, or semantic-reference site.
An exact mapped forbidden file edge can therefore retain a neutral removal
objective. Broader issues produce a manual item exposing sites with fact
identity, line, UTF-8 byte range, expected text, and source SHA-256 when the
provider supplies a nonempty editable span. A zero-width semantic anchor
remains explicit evidence and a non-executable reason, not a fabricated text
edit. This bounds the locations a developer, agent, or structured executor
must inspect without pretending that an aggregated forbidden edge determines
its intended replacement route.

Act first rebuilds the current Map and Verify result. Their project,
input, configuration, producer, policy, and result identities must match the
Plan's source snapshot. Act then applies the prepared create/modify/delete/move
set to an immutable source overlay, reruns discovery and providers from those
bytes, and evaluates the union of item acceptance constraints and preserved
constraints against that isolated after-Map. A `not_satisfied`, `unknown`, or
evaluation failure emits no accepted Act and performs no worktree write.
Satisfied acceptance seals the exact Act bytes and after-state identities.
Apply observes every affected path and native preflight classifies the whole
Act as ready, already satisfied, partially applied, or drifted. A complete
after-state replay succeeds with zero writes; partial application and drift
fail. Ready bytes are replayed transactionally under an exclusive
repository-local lock. Commit failures restore only Act-owned paths;
concurrent changes to those paths are detected and never overwritten.
Archbird does not run project compilers or tests; configure test observations
and build evidence when those results must participate in Verify.

Destructive generated items require a current, complete, exhaustive relation
projection. Unresolved imports or other relation frontiers make the item
non-executable rather than allowing an apparently unused file or symbol to be
removed. Every constraint in the source Verification is checked against the
isolated after-state before commit, including constraints that passed when the
Plan was generated.

Plan ingestion is bounded before expensive work: 64 MiB for canonical Plan JSON
and each source, 4,096 items or touched files, 16 MiB per operation text field,
256 MiB aggregate touched source and patch, 64 MiB per file patch, and 64 KiB
per metadata string. Source coordinates stop at JavaScript's exact integer
limit (`2^53 - 1`) so Python and Node consume the same Plan.

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

Tree-sitter recovery is fact-local. Semantic indexes retain producer, document
coverage, source anchoring, and freshness. Provider conflicts, ambiguity, and
unresolved targets remain explicit.

## Programmatic APIs

### Python

```python
from archbird import Project, compile_plan_json, render_plan_markdown

project = Project.from_repository(".")
map_json = project.map_json(pretty=True)
print(project.map_markdown(max_chars=12_000).decode())
print(project.query_markdown(symbols=["runtime_start"], depth=1).decode())
selection = project.query_json(symbols=["runtime_start"], depth=0)
print(project.source_markdown(
    artifact_json=selection
).decode())
if project.verification_configured:
    verification_json = project.verify_json()
    plan_json = compile_plan_json(
        project, project.map_json(), verification_json
    )
    print(render_plan_markdown(plan_json).decode())
print(project.query_markdown(
    symbols=["runtime_start"], depth=1, view="changes", detail="compact"
).decode())
```

### JavaScript / Node

```js
const { Project, compilePlan, renderPlanMarkdown } = require("archbird");

const project = Project.fromRepository(".");
try {
  console.log(project.mapMarkdown({ maxChars: 12000 }).toString("utf8"));
  const selectionJson = project.queryJson({
    symbols: ["runtime_start"], depth: 0,
  });
  console.log(project.sourceMarkdown({
    artifactJson: selectionJson,
  }).toString("utf8"));
  if (project.verificationConfigured) {
    const mapJson = project.mapJson();
    const verificationJson = project.verifyJson();
    const planJson = compilePlan(project, mapJson, verificationJson);
    console.log(renderPlanMarkdown(planJson).toString("utf8"));
  }
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
  const selectionJson = project.queryJson({ symbols: ["answer"], depth: 0 });
  console.log(project.sourceMarkdown({
    artifactJson: selectionJson,
  }).toString("utf8"));
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
     call Map, Verify, Query, Diff, or workspace APIs. */
  archbird_engine_destroy(engine);
}
```

The public C ABI uses opaque handles, allocator-aware byte buffers, explicit
statuses, and canonical JSON boundaries. It is experimental ABI v0.

### Complete API inventory

Python and Node expose parallel Map, Query, Verify, Plan, and Act capabilities
where their runtimes permit them. The inventories below are checked against
`archbird.__all__` and `Object.keys(require("archbird"))`; host-specific schema,
cache, OKF, observation, and runtime inspection helpers intentionally differ.

<!-- archbird-python-api:start -->
| Python area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map and Query | `analyze_workspace_json`, `audit_map_freshness`, `diff_maps_json`, `export_graph`, `query_map_json`, `query_map_markdown`, `render_map_markdown`, `render_source_markdown`, `resolve_discovery` |
| Projection and policy | `compile_project_configuration`, `compile_query_plan_json`, `evaluate_constraints_json`, `evaluate_projection_json`, `freeze_constraints_json` |
| Plan and Act | `accept_act_json`, `act_overlay`, `act_source_requirements`, `apply_accepted_act`, `compile_plan_json`, `inspect_ast_grep_executable`, `materialize_act_json`, `materialize_ast_grep_operations`, `observe_act_sources`, `observe_plan_sources`, `plan_source_requirements`, `preflight_act_apply`, `render_act`, `render_plan_markdown`, `validate_act`, `validate_plan` |
| Observations and OKF | `analyze_okf_source`, `compile_test_observations`, `export_okf_bundle`, `publish_okf_bundle`, `validate_test_symbol_observations`, `write_okf_bundle` |
| Runtime and schemas | `__version__`, `implementation_digest`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `read_schema`, `schema_names` |
<!-- archbird-python-api:end -->

<!-- archbird-node-api:start -->
| Node area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map and Query | `analyzeWorkspace`, `auditMapFreshness`, `diffMaps`, `exportGraph`, `queryMap`, `queryMapMarkdown`, `renderMapMarkdown`, `renderSourceMarkdown`, `resolveDiscovery` |
| Projection and policy | `compileProjectConfiguration`, `compileQueryPlan`, `evaluateConstraints`, `evaluateProjection`, `freezeConstraints`, `reportConstraints` |
| Plan and Act | `acceptAct`, `actOverlay`, `actSourceRequirements`, `applyAcceptedAct`, `compilePlan`, `materializeAct`, `observeActSources`, `observePlanSources`, `planSourceRequirements`, `preflightActApply`, `renderAct`, `renderPlanMarkdown`, `validateAct`, `validatePlan` |
| Observations and OKF | `analyzeOkfSource`, `compileTestObservations`, `publishOkfBundle` |
| Runtime and planning | `defaultProviderCacheDir`, `defaultProviderCacheMaxBytes`, `discoveryPlan`, `jsonCanonicalize` |
| Runtime metadata | `ENGINE`, `IMPLEMENTATION_SHA256`, `NATIVE_ABI_VERSION`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `PROVIDER_SUPPORT`, `VERSION` |
<!-- archbird-node-api:end -->

`archbird/browser` exports `createBrowserArchbird()`. Browser repository input
is an explicit inventory/byte snapshot, not ambient filesystem access.
`Project.fromFiles()` resolves discovery from supplied bytes. Hosts that receive
large directory inventories can call `Project.discoveryContentPaths()`, read
only those small discovery inputs, call `Project.resolveInventory()`, then read
`resolution.files` and construct the project with
`Project.fromResolvedFiles()`. This is the metadata-first path used by the app.
The resolved facade is:

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
| Engine and structured edits | `archbird_engine_create`, `archbird_engine_destroy`, `archbird_engine_error`, `archbird_engine_error_offset`, `archbird_engine_options_init`, `archbird_engine_options_init_for_input`, `archbird_graph_options_init`, `archbird_implementation_sha256`, `archbird_json_canonicalize`, `archbird_json_pointer_edit`, `archbird_json_pointer_edit_options_init`, `archbird_json_pointer_edit_result_init`, `archbird_json_validate`, `archbird_make_variable_token_edit`, `archbird_make_variable_token_edit_options_init`, `archbird_make_variable_token_edit_result_init`, `archbird_make_variable_token_insert`, `archbird_make_variable_token_insert_options_init`, `archbird_make_variable_token_insert_result_init`, `archbird_unified_diff_options_init` |
| Discovery | `archbird_discovery_add_ignore`, `archbird_discovery_add_path`, `archbird_discovery_create`, `archbird_discovery_destroy`, `archbird_discovery_render`, `archbird_discovery_resolve`, `archbird_discovery_should_descend` |
| Configuration, projections, constraints | `archbird_constraints_evaluate`, `archbird_constraints_freeze`, `archbird_constraints_report`, `archbird_constraints_report_with_blocking`, `archbird_project_configuration_compile`, `archbird_projection_evaluate`, `archbird_projection_render_markdown`, `archbird_query_plan_compile` |
| Project evidence | `archbird_project_add_provider_facts`, `archbird_project_add_source`, `archbird_project_add_test_symbol_observations`, `archbird_project_config_sha256`, `archbird_project_create`, `archbird_project_destroy`, `archbird_project_finalize_providers`, `archbird_project_finalize_sources`, `archbird_project_manifest_sha256`, `archbird_project_map_input_sha256`, `archbird_project_merge_summary`, `archbird_project_provider_count`, `archbird_project_provider_fact_count`, `archbird_project_render_file_facts`, `archbird_project_render_map`, `archbird_project_render_merge_conflicts`, `archbird_project_render_merge_ledger`, `archbird_project_render_provider_facts`, `archbird_project_render_source_markdown`, `archbird_project_scan_builtin`, `archbird_project_scan_builtin_provider`, `archbird_project_scan_builtin_provider_file`, `archbird_project_set_config`, `archbird_project_source`, `archbird_project_source_count`, `archbird_provider_facts_validate`, `archbird_source_manifest_validate`, `archbird_test_symbol_observations_validate` |
| Map, Query, interchange | `archbird_map_diff`, `archbird_map_export_graph`, `archbird_map_freshness`, `archbird_map_query`, `archbird_map_query_markdown`, `archbird_map_query_markdown_view`, `archbird_map_query_markdown_view_with_verification`, `archbird_map_render_markdown`, `archbird_map_render_markdown_view`, `archbird_okf_analyze`, `archbird_okf_publish`, `archbird_unified_diff` |
| Workspace | `archbird_workspace_analyze`, `archbird_workspace_plan` |
| Plan and Act | `archbird_act_accept`, `archbird_act_materialize`, `archbird_act_preflight_apply`, `archbird_act_source_requirements`, `archbird_act_validate`, `archbird_plan_compile`, `archbird_plan_render_markdown`, `archbird_plan_source_requirements`, `archbird_plan_validate` |
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
`workspace`, `verify`, `plan`, `act`, `apply`, `export`, `okf`,
`serve`, `mcp`, `support`.
<!-- archbird-python-cli:end -->

<!-- archbird-node-cli:start -->
Node: `map`, `config`, `query`, `impact`, `diff`, `observe`, `freshness`,
`workspace`, `verify`, `plan`, `act`, `apply`, `export`, `serve`,
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
Concurrent writers use owned atomic temporaries: live or unverifiable writers
are preserved, while abandoned same-execution-domain writes are reclaimed when
the host has a safe process-liveness probe. The Python host supervises bounded
multiprocess CPython-AST worker batches and terminates its workers when a batch
does not return within
`--python-provider-timeout` seconds; this execution policy does not enter
canonical Map identity.

## CI and agent workflow

```bash
archbird map . --progress always \
  --format json --output .archbird/map.json --check
archbird verify \
  --format sarif --output .archbird/architecture.sarif --check
```

For agents:

1. Generate one checked canonical Map before broad exploration.
2. Run the reviewed constraint policy and start from stable constraint and
   requirement IDs.
3. Query bounded context and inspect the exact witnesses used for decisions.
4. Treat candidate/conservative tests as navigation, not execution.
5. Review or edit the generated Plan before invoking Act.
6. Materialize and verify with `archbird act PLAN.json`, review the accepted
   Act, then replay it explicitly with `archbird apply ACT.json`.
7. Regenerate runner evidence after changes when behavioral acceptance depends
   on project-owned observations.
8. Check freshness before treating saved evidence as the live checkout.

## Guarantees, limits, and distribution

- Identical selected source, config, provider implementations, and supplied
  evidence produce byte-identical canonical output under the same Archbird
  implementation.
- Archbird performs no analyzed-project import/execution, network call, model
  call, or agent invocation. Repository mutation occurs only through explicit
  `archbird apply ACT.json`; Plan compilation and Act are non-mutating.
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

Native CMake builds and cppcheck use `BUILD_JOBS=2` by default so complete
gates remain bounded on development hosts without swap. Increase the explicit
bound on larger machines, for example `make verify BUILD_JOBS=8`; outer
`make -jN` controls target scheduling but does not replace this nested-build
limit.

Use `make editable-install PYTHON=/path/to/python` for Python source development
and `make build-c` after C edits. The root submodules are development inputs;
PyPI and npm releases contain generated, content-hashed C snapshots and never
require Git or submodules at installation time. `tools/sync_csrc.py` creates
those publishable snapshots from the pinned gitlinks; generated snapshots are
not a second source of truth.
