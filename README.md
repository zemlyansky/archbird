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

archbird .              # npm: npx archbird .
archbird serve .        # npm: npx archbird serve .
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

## Map and query a repository

Save complete evidence once, then query it without re-analyzing:

```bash
mkdir -p .archbird
archbird map . --format json --pretty \
  --output .archbird/map.json --check

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --depth 1 --max-chars 12000

archbird impact --map .archbird/map.json \
  --path src/runtime.c --depth 2
```

Query selectors accept exact or partial symbols, paths, mapped directories,
globs, layers, components, tests, packages, artifacts, builds, provider
surfaces, parity surfaces, and named entries. `query` is bidirectional by
default; `impact` starts upstream. Occurrence-backed symbol relations are used
before conservative file expansion.

The default is an architecture-first overview for a person or coding agent;
canonical JSON still contains every selected file and mapped fact. Choose the
human projection and its amount of detail independently:

```bash
archbird . --view overview --detail compact       # shortest repository brief
archbird . --view architecture                    # components and connections
archbird . --view audit --detail standard         # complete analysis accounting
archbird . --view audit --full                    # all human-readable Map detail
```

`--compact` and `--full` are aliases for the corresponding `--detail` values.
`--max-chars` is a final output guard; it never changes the canonical Map.
Query profiles (`exact`, `change`, `architecture`, `audit`), per-kind quotas,
route provenance/confidence, and candidate/conservative policies control which
focused evidence is shown.

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
archbird serve . --config archbird.json
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

The complete configuration vocabulary is:

| Section | Purpose |
| --- | --- |
| `layers`, `components` | selected source/provider groups and reviewed architecture groupings |
| `packages`, `builds`, `artifacts` | manifests, public entrypoints, Make/npm routes, logical outputs and loaders |
| `bridges` | declared/used/implemented ABI, binding, or message surfaces |
| `tests` | static cases, reviewed `case_routes`, and generated-source relations |
| `named_entries`, `parity` | configured entrypoint protocols and reviewed surface relationships |
| `indexes` | one or more SCIP indexes with prefixes and position encoding |
| `checks`, `limits` | basic Map-presence requirements and bounded resource policy |

Selectors are segment-aware: `src/*.c` matches immediate children and
`src/**/*.c` is recursive. Components group selected files; they do not discover
new source. `route_to` records broad asserted intent; `case_routes` is
case-specific. Patterns use the pinned `archbird-pcre2-v1` contract rather than
Python `re` or JavaScript `RegExp`.

The block above is mirrored by
[`examples/minimal.archbird.json`](examples/minimal.archbird.json). A complete
native/Python/TypeScript/package/build/test example is
[`examples/quickstart.archbird.json`](examples/quickstart.archbird.json). The
strict schema is [`schema/archbird.schema.json`](schema/archbird.schema.json).

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
archbird verify --config architecture.verify.json \
  --format junit --output .archbird/architecture.junit.xml --check
```

Verify supports set/value equality, mapped names/values, directional subsets,
cardinality, required/forbidden/allowed edges, acyclicity, minimum test routes,
and behavioral-attestation equality. Extractors cover literal facts, symbols,
values, component/file edges, test routes, provider surfaces, Python enums and
sets, C enums/designated initializers/macros, and supplied attestations.

Facts have three non-interchangeable provenances:

| Provenance | Meaning | Examples |
| --- | --- | --- |
| `derived` | deterministically extracted | symbols, edges, registrations |
| `asserted` | reviewed intent | requirements, mappings, waivers |
| `observed` | project-runner output | behavior vectors, exact test-to-symbol hits |

Comparison, freshness, applicability, disposition, and baseline state are
independent; stale, unknown, unsupported, or inapplicable evidence never turns
into pass. Every check has a stable ID, owner, severity, rationale, optional
requirement IDs/tags, cited evidence, and stable finding fingerprints.

Multi-project suites support source locks, explicit name/value mappings,
intentional supersets, reviewed divergences, baselines, waivers, and local
project-root overrides that do not enter canonical evidence. Use project-owned
attestations for behavior; similar names alone are not semantic equivalence.
See [`examples/tinygrad-polygrad.verify.json`](examples/tinygrad-polygrad.verify.json)
for a porting contract.

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
archbird verify --config architecture.verify.json --format json \
  --output .archbird/before.verify.json

archbird plan --verification .archbird/before.verify.json \
  --finding FINGERPRINT --output .archbird/change.proposal.json

archbird contract --proposal .archbird/change.proposal.json \
  --objective "Restore the reviewed public surface" \
  --owner core --rationale "Reviewed evidence and implementation strategy" \
  --preserve-all --output .archbird/change.contract.json

# An external executor edits, builds, and tests; then regenerate Verify.
archbird verify --config architecture.verify.json --format json \
  --output .archbird/after.verify.json

archbird verify-plan \
  --proposal .archbird/change.proposal.json \
  --contract .archbird/change.contract.json \
  --before-verification .archbird/before.verify.json \
  --after-verification .archbird/after.verify.json --check
```

Proposals separate required postconditions, evidence-backed candidates,
preserved checks, coverage, and unknowns. Contracts are immutable asserted
review. Results distinguish satisfied, missing, unexpected, unknown, stale,
and superseded. Candidate paths are advice, not write authorization.

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

## APIs

### Python

```python
from archbird import Project

project = Project.from_repository(".")
map_json = project.map_json(pretty=True)
print(project.map_markdown(max_chars=12_000).decode())
print(project.query_markdown(symbols=["runtime_start"], depth=1).decode())
```

### JavaScript / Node

```js
const { Project } = require("archbird");

const project = Project.fromRepository(".");
try {
  console.log(project.mapMarkdown({ maxChars: 12000 }).toString("utf8"));
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

## Interchange and command surface

Canonical Archbird JSON is authoritative. Optional inputs/projections are:

| Format | Direction | Role |
| --- | --- | --- |
| SCIP | input | semantic definitions, references, relationships |
| OKF v0.1 | Python input/output; Node library projection | browsable knowledge bundle; prose never becomes checks |
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

The complete commands are `map`, `config show|init`, `query`, `impact`,
`freshness`, `diff`, `workspace`, `verify`, `plan`, `contract`, `verify-plan`,
`export json|graphml|mermaid`, and `serve`; Python adds filesystem
`okf validate|index|query` and `export okf`, while npm adds `support`. Use
`archbird COMMAND --help` for flags. Exit status is 0 for success, 1 when
requested `--check` blocks, and 2 for invalid input/configuration.

## CI and agent workflow

```bash
archbird map . --format json --output .archbird/map.json --check
archbird verify --config architecture.verify.json \
  --format sarif --output .archbird/architecture.sarif --check
```

For agents:

1. Generate one checked canonical Map before broad exploration.
2. Run the reviewed Verify suite and start from stable check/requirement IDs.
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
./archbird .

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
