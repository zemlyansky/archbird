---
name: archbird
description: Use Archbird to understand unfamiliar codebases, trace dependencies and impact, inspect source-backed architecture evidence, check architectural constraints, and plan coordinated structural changes. Use for broad exploration, multi-file or cross-language work, architecture questions, migration review, or when the relevant files are unknown. Do not use for narrow work whose files are already known.
license: Apache-2.0
metadata:
  author: zemlyansky
  version: "0.0.4"
---

# Archbird

Use Archbird to select and verify repository evidence before reading broad
amounts of source. Archbird does not replace direct source inspection, runtime
tests, or project-specific knowledge.

## Confirm the interface

Prefer configured `archbird_*` MCP tools for repeated read-only exploration.
Otherwise use the installed CLI:

```bash
command -v archbird
archbird --version
```

If `archbird` is absent but the repository contains
`node_modules/.bin/archbird`, replace `archbird` below with
`npx --no-install archbird`. Do not download or install software without user
authorization. Run `archbird COMMAND --help` when syntax is uncertain; the
installed version is authoritative.

## Choose the root

Use the smallest repository or subproject containing the relevant code and its
`archbird.json` or `.git`. Do not map a workspace aggregator merely because it
contains agent instructions.

Before a broad Map, inspect the proposed root:

```bash
pwd
test -f archbird.json && echo archbird-config || true
find . -maxdepth 2 \( -name archbird.json -o -name .git -o -name AGENTS.md \) -print | sed -n '1,40p'
```

If independent child repositories appear, select the relevant child. If the
correct scope cannot be established, use direct source tools or ask for the
root instead of starting an expensive scan.

## Orient, narrow, then read

Start broad work with a bounded overview:

```bash
archbird map . --view overview --detail standard --max-chars 12000 --check
```

If the identity is unknown, use concise repository terms. Search is lexical;
its ranking supplies advisory seeds, not proven semantic relationships:

```bash
archbird query . --search 'provider registry' --max-chars 12000 --check
```

Once a path and symbol are known, request their exact neighborhood:

```bash
archbird query . --symbol 'PATH:SYMBOL' --depth 1 --test-depth 1 --max-chars 12000 --check
```

Read hash-bound declaration source with `--view source`. Dump a complete file
only after narrowing it; `--dump` is intentionally unbounded and cannot be
combined with `--max-chars`:

```bash
archbird query . --symbol 'PATH:SYMBOL' --view source --detail standard --max-chars 12000 --check
archbird query . --path PATH --dump --check
```

Use `archbird path SOURCE TARGET --check` for an explicit connectivity
question. A candidate-only or incomplete route is `unknown`, not a proven path.

## Measure and explain hotspots

Use the exhaustive Metrics view to locate measured structural tails:

```bash
archbird map . --view metrics
archbird query . --symbol 'PATH:SYMBOL' --view metrics --detail full
```

Metrics are measurements, not quality scores. Compare distributions and
evidence completeness before prioritizing work. Full focused detail lists the
exact control-flow constructs or graph relations contributing to a value; read
that evidence before changing code.

## Check architecture and changes

Run exhaustive project constraints before and after architecture-sensitive
work:

```bash
archbird verify --root . --check
```

Query a Git change for bounded implementation context:

```bash
archbird query . --git-diff HEAD~1 --view changes --max-chars 12000 --check
```

Use `archbird diff BEFORE AFTER` for whole-Map structural comparison. Operands
may be repository roots or saved Maps. Add reviewed correspondence only when
comparing different identities or implementations; never infer cross-language
equivalence from similar names.

## Reuse evidence safely

Save complete evidence when later work needs the same snapshot:

```bash
archbird map . --format json --pretty --output .archbird/map.json --check
archbird freshness --root . --snapshot .archbird/map.json --check
```

Run freshness immediately before reusing a saved Map. Query output may rank,
collapse, and truncate context. Verification projections must remain
exhaustive.

For a long read-only session, configure the host to launch
`archbird mcp --root .`. Use its generation-bound Map, Projection, Query, Path,
source, Verify, and Diff tools. MCP does not authorize repository mutation.

## Preserve evidence meaning

- Exact evidence identifies a resolved target.
- Candidate and conservative evidence are useful navigation, not proof.
- Unresolved, stale, partial, or unsupported evidence remains unknown.
- Static test routes do not prove execution. Only project-owned runner
  observations are observed evidence.
- A successful bounded presentation does not mean omitted records do not exist;
  inspect its selection and completeness ledger.

If output conflicts with source, inspect the source directly and use it as the
authority. Record the Archbird version, exact command, error, and minimal
counterexample for reporting. Never hide uncertainty or manufacture evidence.

## Plan reviewed structural changes

`archbird plan` derives a source-locked editable plan from current constraint
issues or reviewed intent. Manual items mean the evidence does not determine an
edit; do not invent one merely to close the plan.

`archbird act PLAN.json` materializes submitted edits in an isolated after-state
and checks the result. It does not write the working repository.

Run `archbird apply ACT.json` only when the user explicitly authorizes mutation
and the exact accepted Act has been reviewed. Apply revalidates source locks;
never bypass them.
