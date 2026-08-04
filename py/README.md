# Archbird for Python

**Map codebases. Verify architecture. Plan and apply structural changes.**

Archbird scans a repository and builds a deterministic map of its files,
symbols, dependencies, public interfaces, tests, build routes, and components.
Use it to understand unfamiliar code, give coding agents focused context,
enforce reviewed architecture constraints in CI, compare ports or frontends,
and check that coordinated changes produced the required structural result.

```bash
python -m pip install archbird

archbird map .  # map the current repository
archbird serve  # explore it in the local web application
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

Every result links back to the source, configuration, or test data used to
produce it. Missing or uncertain information is shown instead of guessed.

Map works without configuration. Add Verify when you want automated
architecture constraints. Plan derives only edits established by current
evidence and exposes underdetermined work as manual items. Act materializes and
checks exact edits against an isolated after-state without writing. Apply
revalidates source locks and replays only an accepted Act.

`archbird` and `archbird .` remain supported shortcuts for mapping the current
repository. The explicit `archbird map` form is useful in scripts and alongside
the other stage commands.

## Command-line workflow

The CLI follows the product stages directly. Run commands from the repository
root; `archbird.json` is discovered automatically when present.

### Map a repository

Map builds the reusable repository model. It works without configuration:

```bash
archbird map .
archbird map . --view architecture \
  --group-by component --level file --relations imports,calls
archbird map . --view tests --group-by directory
archbird map . --view evidence --detail full
```

The default Markdown is an architecture-first overview. `--view` chooses the
subject, `--group-by` organizes entities, `--level` chooses component, file, or
symbol nodes, and `--relations` selects graph relations. `--detail` changes
presentation density only; canonical JSON remains exhaustive. Unsupported,
ignored, or oversized inputs are reported separately from completeness of the
selected graph.

Save the complete Map when later operations must use the same repository
evidence:

```bash
mkdir -p .archbird
archbird map . --format json --pretty \
  --output .archbird/map.json --check
archbird config show . --pretty \
  --output .archbird/resolution.json --check
```

Archbird excludes `.archbird/**` by default, so its generated artifacts do not
change discovery. `--progress auto` updates one terminal line during longer
interactive runs and stays silent when output is piped; use `always` or
`never` to override it.

### Query focused context

Query selects and ranks a task-sized neighborhood from the Map:

```bash
archbird query . \
  --symbol 'src/runtime.c:runtime_start' \
  --depth 1 --test-depth 1 --max-chars 12000 --check

archbird query . --search 'provider registry' \
  --search-limit 8 --max-chars 12000 --check

archbird impact . --path src/runtime.c --depth 2 --check
```

`--search` is deterministic lexical retrieval, not natural-language or
semantic search. Use concise repository vocabulary, then switch to an exact
`--symbol`, `--path`, `--component`, or `--test` selector. Candidate and
conservative test routes are navigation evidence, not proof that a test ran.

Render hash-checked source from the same selection:

```bash
archbird query . \
  --symbol 'src/runtime.c:runtime_start' \
  --view source --detail standard --max-chars 12000 --check

archbird query . --path src/runtime.c --dump --check
```

Standard source detail expands exact declarations and directly selected files.
`--dump` returns the complete selected file and intentionally cannot be
combined with `--max-chars`. Changed bytes, missing extents, non-UTF-8 input,
and terminal-control bytes are rejected rather than silently rendered.

For a current change set:

```bash
archbird query --git-diff HEAD --view changes --detail compact --check
archbird query --git-diff HEAD --view changes \
  --verification-result .archbird/verify.json --check
```

The changes view groups seeds, affected code, strongest routes, ranked tests,
packages, builds, artifacts, uncertainty, and collapsed evidence. Git
deletions and paths outside the Map remain explicit; untracked files require an
explicit `--path`.

### Find a connection with Path

Path answers one explicit graph-connectivity question:

```bash
archbird path 'src/cli.c' 'src/runtime.c' \
  --root . --relation calls --direction downstream --check

archbird path 'src/cli.c' 'src/runtime.c' \
  --map .archbird/map.json \
  --relation calls --direction downstream --check
```

It returns bounded shortest witnesses with typed relations, direction,
provenance, evidence state, semantic resolution, and completeness. A `found`
result requires a current source-evidenced route. Candidate-only,
stale, incomplete, or depth-bounded connectivity remains `unknown`; `--check`
does not turn it into proof.

### Verify architecture

Reviewed architecture policy belongs in the `constraints` collection of the
same `archbird.json` that defines project structure. Typed constraints infer
their exhaustive Map projections; primitive assertions can use inline literals,
observations, or named/inline projections. The project configuration described
below therefore needs no second suite file.

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
archbird plan --format markdown
archbird plan --output .archbird/plan.json
archbird plan CORE-PUBLIC-API --output .archbird/plan.json
archbird plan CORE-PUBLIC-API --rename old_api=new_api \
  --output .archbird/plan.json
archbird plan FFI-SURFACE --git-diff HEAD \
  --output .archbird/plan.json
archbird map --format json --output .archbird/before-map.json
# Or, after saving an explicit Map before a partial migration:
archbird plan FFI-SURFACE --before-map .archbird/before-map.json \
  --output .archbird/plan.json
archbird act .archbird/plan.json
archbird act .archbird/plan.json \
  --submit IMPLEMENT_ITEM=reviewed-module.py \
  --submit TEST_ITEM=reviewed-test.py
archbird act .archbird/plan.json --format patch
archbird act .archbird/plan.json --format json \
  --output .archbird/act.json
archbird apply .archbird/act.json
```

The neutral `redirect_dependency` operator stores an exhaustive edge
ProjectionPlan, the relation and symbol identities, and affected source paths.
It stores no source ranges or replacement text. Act reevaluates the projection
once and dispatches its typed evidence by mapped source language. The native C
executor resolves exact declaration, definition, include, and call evidence.
The native Python executor requires one exact imported binding, exact
CPython-AST call evidence, and an already observed import-module spelling for
the replacement; it preserves explicit local aliases. Native Act also contains
an ECMAScript grounding executor, but it requires exact TypeScript-compiler
reference evidence. This frontend's default JavaScript/TypeScript providers are
syntax-only, so it rejects such a redirect unless the evaluated Map carries the
required semantic evidence. Incomplete evidence, ambiguous definitions,
missing observed include/import routes, multi-name imports, and non-unique
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
scope. Act reevaluates the same complete evidence and lets the native Python
executor materialize exact declaration, import, export, binding, and reference
edits from CPython AST evidence, including qualified imported-attribute
references and literal `__all__` export tokens. Qualified identities may
rename only their terminal identifier while preserving the same enclosing
identity. Unrelated same-name declarations outside the selected projection
scope are not renamed. Public alias assignments are separate reviewed surface
identities, not internal declaration occurrences. Candidate or unresolved
calls, duplicate targets, unsupported inputs, and lexical-only C/C++ call
binding block Act instead of producing a partial rename.

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
through `Project.with_source_overlay()`, deriving a fresh Map and every
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

```bash
archbird serve
```

`serve` prints a loopback URL immediately. The local application presents the
Map, source witnesses, architecture graph, focused queries, constraints,
snapshots, and diffs while a background worker watches the repository. Only a
valid new generation replaces the last good Map. The page uses the native
Python host for analysis; it does not download the canonical Map unless the
user explicitly saves it.

### Reuse saved evidence safely

Saved Maps preserve one analyzed state; they do not prove that the checkout is
still unchanged:

```bash
archbird freshness . --snapshot .archbird/map.json \
  --output .archbird/freshness.json --check

archbird query --map .archbird/map.json \
  --symbol 'src/runtime.c:runtime_start' --check

archbird verify --map .archbird/map.json \
  --resolution .archbird/resolution.json --check
```

Checked saved-artifact operations require a compatible current producer.
Freshness independently compares the saved source/config evidence with a newly
derived live Map. A saved Map contains mapped facts rather than the full
discovery inventory, so inventory-sensitive constraints also need its matching
configuration-resolution artifact.

## Python API

The Python API exposes the same stages without subprocesses.

### Map, Query, and Path

Use `Project` for the normal repository workflow:

```python
from archbird import (
    Project,
    audit_map_freshness,
    compile_plan_json,
    render_plan_markdown,
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
connection = project.path_markdown(
    {"kind": "file", "patterns": ["src/cli.c"]},
    {"kind": "file", "patterns": ["src/runtime.c"]},
    relations=["calls"],
    direction="downstream",
)

print(overview.decode())
print(context.decode())
print(source.decode())
print(connection.decode())
print(audit_map_freshness(map_json, project.map_json()).decode())

if project.verification_configured:
    verification_json = project.verify_json()
    plan_json = compile_plan_json(
        project,
        project.map_json(),
        verification_json,
    )
    print(render_plan_markdown(plan_json).decode())
```

`Project.from_repository()` applies discovery, project configuration, and
explicit options. `Project.from_config()` requires one reviewed configuration.
Canonical JSON methods return stable artifact bytes; Markdown and graph outputs
are presentation views.

### Verify, Plan, Act, and Apply

`Project.verify_json()` evaluates configured constraints exhaustively.
`compile_plan_json()` delegates Plan derivation to the native core. Its optional
`before_map_json` input enables identity-checked residual planning; Python
performs no Map comparison or action inference.
`render_plan_markdown()` presents the same validated Plan as a concise task
packet; it does not create or modify an artifact.
`materialize_act_json()` produces exact binary-safe transitions from a Plan.
Its optional `executor_submissions_json` input supplies reviewed full-file
content for exact unresolved symbol, test-route, required-path, or dependency
items; the corresponding `plan_source_requirements()` input asks the host for
the exact state required by each objective. Native Act then chooses create or
replace where the objective permits either. Submissions are ephemeral executor
input and never mutate Plan.
`accept_act_json()` seals them only after callers supply the fresh isolated
after-Map and Verification. `preflight_act_apply()` returns `ready` or
`already_satisfied` after comparing newly observed sources with the complete
sealed before/after states; partial application and unrelated drift fail. The
explicit filesystem helpers `observe_plan_sources()`,
`act_overlay()`, `run_act_gates()`, `render_act()`, and
`apply_accepted_act()` provide that host transport. Reviewed `gates` from
`archbird.json` execute as direct argument arrays over the copied after-state;
the native core rejects incomplete or non-passing result ledgers before
acceptance. All Plan interpretation, edit materialization, gate-result
validation, and acceptance remain in the native core.

Saved-Map helpers `query_map_json()`, `query_map_markdown()`,
`path_map_json()`, and `path_map_markdown()` accept
`producer_policy="compatible"` or `"current"`. Configuration, projection,
QueryPlan, constraint, baseline, observation, workspace, Plan, Act, graph, and
OKF functions expose the same canonical artifacts as the CLI.
`render_path_markdown()` presents an already evaluated canonical Path, so a
checked host can evaluate once and select JSON or Markdown without changing the
witness. Candidate-only Path witnesses remain `unknown`; Query route metadata
keeps evidence state, resolution counts, provenance count, and completeness
separate.

The optional ast-grep adapter is planning-time only. A reviewed integration
pins the executable SHA-256 and version, then
`materialize_ast_grep_operations()` translates a bounded non-mutating preview
into ordinary source-locked Plan operations. Act itself never invokes ast-grep.

`schema_names()` lists every bundled JSON schema and `read_schema()` returns its
exact bytes. Filesystem OKF parsing/writing is Python-specific because it uses
the optional YAML/CommonMark adapter; normalized OKF analysis and publication
remain shared with Node and C.

### API inventory

<!-- archbird-python-api:start -->
| Area | Public names |
| --- | --- |
| Repository model | `Project`, `Source`, `Workspace` |
| Map, Query, and Path | `analyze_workspace_json`, `audit_map_freshness`, `diff_maps_json`, `export_graph`, `path_map_json`, `path_map_markdown`, `query_map_json`, `query_map_markdown`, `render_map_markdown`, `render_path_markdown`, `render_source_markdown`, `resolve_discovery` |
| Projection and policy | `compile_project_configuration`, `compile_query_plan_json`, `evaluate_constraints_json`, `evaluate_projection_json`, `freeze_constraints_json` |
| Plan and Act | `accept_act_json`, `act_overlay`, `act_source_requirements`, `apply_accepted_act`, `compile_plan_json`, `inspect_ast_grep_executable`, `materialize_act_json`, `materialize_ast_grep_operations`, `observe_act_sources`, `observe_plan_sources`, `plan_source_requirements`, `preflight_act_apply`, `render_act`, `render_plan_markdown`, `run_act_gates`, `validate_act`, `validate_plan` |
| Observations and OKF | `analyze_okf_source`, `compile_test_observations`, `export_okf_bundle`, `publish_okf_bundle`, `validate_test_symbol_observations`, `write_okf_bundle` |
| Runtime and schemas | `__version__`, `implementation_digest`, `PATTERN_CONTRACT`, `PATTERN_CONTRACT_VERSION`, `PATTERN_ENGINE`, `PATTERN_OPTIONS`, `PATTERN_UNICODE`, `read_schema`, `schema_names` |
<!-- archbird-python-api:end -->

## MCP for coding agents

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

MCP mirrors the read-only part of the workflow:

- Map and reusable projections;
- Query, Path, and hash-checked source;
- Verify and Diff.

It deliberately does not expose Plan, Act, or Apply as unattended agent tools;
those remain explicit reviewed CLI or Python API operations.

## Agent workflow

Copy this compact policy into a project's `AGENTS.md`, `CLAUDE.md`, or
equivalent agent instructions:

```text
Use Archbird before broad source exploration.

- Start with `archbird map . --view overview --detail standard --max-chars
  12000 --check`.
- When an exact identity is known, use `archbird query . --symbol
  'PATH:SYMBOL' --depth 1 --test-depth 1 --max-chars 12000 --check`.
- When the identity is unknown, use `archbird query . --search 'CONCISE
  REPOSITORY TERMS' --max-chars 12000 --check`. Search is lexical and advisory.
- Read an exact declaration with `archbird query . --symbol 'PATH:SYMBOL'
  --view source --detail standard --max-chars 12000 --check`. Read one complete
  file with `archbird query . --path PATH --dump --check`; do not combine
  `--dump` with `--max-chars`.
- Use `archbird path SOURCE TARGET --check` for explicit connection questions.
  Candidate-only or incomplete connectivity remains `unknown`.
- Run `archbird verify --root . --check` before and after
  architecture-sensitive work. Treat static test routes as navigation, not
  proof of execution.
- Check `archbird freshness --root . --snapshot .archbird/map.json --check`
  before reusing a saved Map.
- Prefer `archbird mcp --root .` for repeated read-only agent exploration.
- Review generated Plans and accepted Acts. Never run `archbird apply` unless
  repository mutation is explicitly authorized and the exact Act was reviewed.
- If Archbird disagrees with source, inspect source directly and report a
  general reproducer; never hide uncertainty or manufacture observed evidence.
```

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

Map uses repository-local C/C++ include search paths from compilation
databases in compiler order. Each translation unit's context follows its
literal includes through reached headers, and variants must agree on one
selected target. Resolved edges cite only the repository-relative database
path; external roots and absolute machine paths remain private.

The embedded config is mirrored by `examples/minimal.archbird.json`; the
source repository also contains a complete package/build/test example in
`examples/quickstart.archbird.json`.

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
GraphML and Mermaid are deterministic projections. Verification results can
render SARIF or JUnit; Plan and Act remain canonical JSON artifacts.

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

## Reference, limits, and development

<!-- archbird-python-cli:start -->
The command names are `map`, `config`, `query`, `impact`, `path`, `diff`, `observe`,
`freshness`, `workspace`, `verify`, `plan`, `act`, `apply`,
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
