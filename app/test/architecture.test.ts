import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { performance } from "node:perf_hooks";
import test from "node:test";

import { parseProjectionResult, type ProjectionResult } from "../src/artifacts/projection";
import {
  mapGraphProjectionPlan,
  presentGraphProjection,
} from "../src/graph/presentation";
import {
  composeArchitectureGraph,
  directoryPresentationId,
  edgePresentation,
  orientGraphEdges,
} from "../src/graph/explore";

type Item = ProjectionResult["fact"]["items"][number];
const require = createRequire(import.meta.url);
const { mapProjectionRequest } = require("../../js/src/map-view.js") as {
  mapProjectionRequest(options: {
    groupBy: string;
    view: string;
  }): { definition: Record<string, unknown> };
};

function item(
  key: string,
  label: string,
  attributes: Record<string, unknown>,
): Item {
  return {
    attributes,
    evidence: [],
    key,
    label,
    message: "",
    state: "current",
    value: null,
  };
}

function node(
  path: string,
  language: string,
  components: string[] = [],
): Item {
  return item(`node:${path}`, path, {
    components,
    entity_kind: "file",
    id: `file:${path}`,
    language,
    layer: `auto-${language}`,
    membership: components.length ? "exclusive" : "unassigned",
    path,
    record_kind: "node",
  });
}

function group(
  kind: "component" | "directory" | "layer",
  id: string,
  label: string,
  origin: "configured" | "discovered" | "unassigned" = "discovered",
  attributes: Record<string, unknown> = {},
): Item {
  return item(`group:${id}`, label, {
    ...attributes,
    entity_kind: kind,
    group_by: kind,
    id,
    origin,
    record_kind: "group",
  });
}

function membership(groupId: string, path: string): Item {
  return item(`membership:${groupId}:${path}`, path, {
    entity_kind: "membership",
    group: groupId,
    id: `${groupId}:${path}`,
    node: `file:${path}`,
    record_kind: "membership",
  });
}

function nodeMembership(groupId: string, nodeId: string): Item {
  return item(`membership:${groupId}:${nodeId}`, nodeId, {
    entity_kind: "membership",
    group: groupId,
    id: `${groupId}:${nodeId}`,
    node: nodeId,
    record_kind: "membership",
  });
}

function relation(source: string, target: string): Item {
  return item(`relation:${source}:${target}`, `${source} imports ${target}`, {
    entity_kind: "relation",
    family: "imports",
    names: ["public_api"],
    record_kind: "relation",
    relation_kind: "import",
    source: `file:${source}`,
    target: `file:${target}`,
    witness_count: 1,
  });
}

function peripheral(kind: string, id: string, label: string): Item {
  return item(`node:${id}`, label, {
    entity_kind: kind,
    id,
    record_kind: "node",
  });
}

function typedRelation(
  family: string,
  kind: string,
  source: string,
  target: string,
): Item {
  return item(`relation:${family}:${source}:${target}`, `${source} ${kind} ${target}`, {
    entity_kind: "relation",
    family,
    names: [],
    record_kind: "relation",
    relation_kind: kind,
    source,
    target,
    witness_count: 1,
  });
}

function groupRelation(
  source: string,
  target: string,
  canonicalRelationKeys: string[],
  names: string[],
): Item {
  return item(
    `group-relation:${source}:${target}`,
    `${source} imports ${target}`,
    {
      canonical_relation_keys: canonicalRelationKeys,
      entity_kind: "group_relation",
      family: "imports",
      names,
      record_kind: "group_relation",
      relation_count: canonicalRelationKeys.length,
      relation_kind: "imports",
      relation_kinds: ["import"],
      source,
      target,
      witness_count: canonicalRelationKeys.length,
    },
  );
}

function result(
  groupBy: "component" | "directory" | "layer",
  items: Item[],
  classification: "complete" | "incomplete" | "unknown" = "complete",
): ProjectionResult {
  const completeness = {
    classification,
    exhaustive: classification === "complete",
    truncated: false,
  };
  return {
    artifact: "projection-result",
    completeness,
    definition: {
      group_by: groupBy,
      level: "file",
      select: "graph",
    },
    fact: {
      completeness,
      items,
      message: "",
      name: `app-${groupBy}`,
      project: "demo",
      shape: "graph",
      state: classification === "complete" ? "current" : "unknown",
    },
    id: `app-${groupBy}`,
    schema_version: 1,
  };
}

test("zero-config graph projection uses repository areas without inventing components", () => {
  const projection = result("directory", [
    group("directory", "directory:.", "."),
    group("directory", "directory:js", "js"),
    group("directory", "directory:py", "py"),
    node("js/index.js", "javascript"),
    node("js/scripts/build_wheel.py", "python"),
    node("py/pkg/api.py", "python"),
    node("Makefile", "make"),
    membership("directory:js", "js/index.js"),
    membership("directory:js", "js/scripts/build_wheel.py"),
    membership("directory:py", "py/pkg/api.py"),
    membership("directory:.", "Makefile"),
    relation("js/index.js", "py/pkg/api.py"),
    item("ledger:imports", "imports", {
      collapsed: 0,
      entity_kind: "relation",
      family: "imports",
      id: "imports",
      record_kind: "ledger",
      unknown: 0,
    }),
  ]);
  const architecture = presentGraphProjection(projection, "architecture");
  assert.equal(architecture.grouping, "directory");
  assert.equal(architecture.hasConfiguredComponents, false);
  assert.deepEqual(
    architecture.overview.nodes.map((row) => [row.kind, row.label]),
    [
      ["root", "Repository root"],
      ["directory", "js/"],
      ["directory", "py/"],
    ],
  );
  assert.ok(architecture.overview.nodes.every((row) =>
    row.attributes.origin === "inferred"
    && row.attributes.presentation_role === "group"));
  assert.equal(
    architecture.overview.nodes.some((row) => row.kind === "component"),
    false,
  );
  assert.equal(architecture.overview.edges[0].names[0], "public_api");
  assert.deepEqual(architecture.overview.omissions[0], {
    collapsed: 0,
    entity_kind: "relation",
    family: "imports",
    id: "imports",
    record_kind: "ledger",
    unknown: 0,
  });

  const js = architecture.overview.nodes.find((row) => row.label === "js/");
  assert.ok(js);
  assert.deepEqual(js.attributes.languages, ["javascript", "python"]);
  const directories = composeArchitectureGraph(
    architecture.overview,
    architecture.files,
    new Map(),
    {
      expandedGroups: new Set([js.id]),
      expandedFiles: new Set(),
      hidden: new Set(),
      revealedSymbols: new Set(),
    },
  );
  const directory = directories.nodes.find((row) =>
    row.kind === "directory" && row.identity === "js/scripts/");
  assert.ok(directory);
  const expanded = composeArchitectureGraph(
    architecture.overview,
    architecture.files,
    new Map(),
    {
      expandedGroups: new Set([
        js.id,
        directoryPresentationId(js.id, "js/scripts"),
      ]),
      expandedFiles: new Set(),
      hidden: new Set(),
      revealedSymbols: new Set(),
    },
  );
  const file = expanded.nodes.find((row) =>
    row.identity === "js/scripts/build_wheel.py");
  assert.equal(file?.kind, "file");
  assert.equal(file?.parent, directory.id);
  assert.equal(
    expanded.nodes.find((row) => row.identity === "js/index.js")?.parent,
    js.id,
  );
});

test("high-cardinality grouped relations aggregate once rather than quadratically", () => {
  const count = 3_000;
  const items: Item[] = [
    group("directory", "directory:left", "left"),
    group("directory", "directory:right", "right"),
  ];
  for (let index = 0; index < count; index += 1) {
    const source = `left/source-${index}.c`;
    const target = `right/target-${index}.h`;
    items.push(
      node(source, "c"),
      node(target, "c"),
      membership("directory:left", source),
      membership("directory:right", target),
      item(`relation:${index}`, `${source} imports ${target}`, {
        entity_kind: "relation",
        family: "imports",
        names: [`name-${index}`],
        record_kind: "relation",
        relation_kind: "import",
        source: `file:${source}`,
        target: `file:${target}`,
        witness_count: 1,
      }),
    );
  }
  const started = performance.now();
  const architecture = presentGraphProjection(
    result("directory", items),
    "architecture",
  );
  const elapsed = performance.now() - started;
  assert.equal(architecture.overview.edges.length, 1);
  assert.equal(architecture.overview.edges[0].names.length, count);
  assert.equal(
    architecture.overview.edges[0].attributes?.witness_count,
    count,
  );
  assert.equal(
    (
      architecture.overview.edges[0].attributes
        ?.canonical_edge_ids as unknown[]
    ).length,
    count,
  );
  assert.ok(
    elapsed < 2_000,
    `grouped relation aggregation exceeded its linear scaling budget: ${elapsed.toFixed(1)} ms`,
  );
});

test("current graph projections consume core group relations without regrouping raw edges", () => {
  const first = relation("consumer/first.c", "provider/api.h");
  const second = relation("consumer/second.c", "provider/api.h");
  const projection = result("directory", [
    group("directory", "directory:consumer", "consumer"),
    group("directory", "directory:provider", "provider"),
    node("consumer/first.c", "c"),
    node("consumer/second.c", "c"),
    node("provider/api.h", "c"),
    membership("directory:consumer", "consumer/first.c"),
    membership("directory:consumer", "consumer/second.c"),
    membership("directory:provider", "provider/api.h"),
    first,
    second,
    groupRelation(
      "directory:consumer",
      "directory:provider",
      [first.key, second.key],
      ["first_api", "second_api"],
    ),
  ]);
  const architecture = presentGraphProjection(projection, "architecture");
  assert.equal(architecture.overview.edges.length, 1);
  assert.deepEqual(
    architecture.overview.edges[0].attributes?.canonical_edge_ids,
    [first.key, second.key],
  );
  assert.equal(architecture.overview.edges[0].attributes?.witness_count, 2);
  assert.deepEqual(
    architecture.overview.edges[0].names,
    ["first_api", "second_api"],
  );
});

test("current intra-group relations do not become aggregate self-loops", () => {
  const projection = result("directory", [
    group("directory", "directory:src", "src"),
    node("src/consumer.c", "c"),
    node("src/provider.h", "c"),
    membership("directory:src", "src/consumer.c"),
    membership("directory:src", "src/provider.h"),
    relation("src/consumer.c", "src/provider.h"),
  ]);
  const architecture = presentGraphProjection(projection, "architecture");
  assert.deepEqual(architecture.overview.edges, []);
});

test("configured and unassigned component groups remain semantically distinct", () => {
  const architecture = presentGraphProjection(result("component", [
    group("component", "component:runtime", "runtime", "configured", {
      description: "Runtime services",
      symbol_count: 7,
    }),
    group(
      "component",
      "component:unassigned:tools",
      "tools",
      "unassigned",
    ),
    node("src/core.c", "c", ["runtime"]),
    node("tools/generate.py", "python"),
    membership("component:runtime", "src/core.c"),
    membership("component:unassigned:tools", "tools/generate.py"),
    relation("tools/generate.py", "src/core.c"),
  ]), "architecture");
  assert.equal(architecture.grouping, "component");
  assert.equal(architecture.hasConfiguredComponents, true);
  assert.deepEqual(
    architecture.overview.nodes.map((row) => [
      row.kind,
      row.attributes.origin,
    ]).sort(),
    [
      ["component", "configured"],
      ["directory", "inferred"],
    ],
  );
  assert.equal(architecture.overview.edges.length, 1);
  const runtime = architecture.overview.nodes.find((row) =>
    row.label === "runtime");
  assert.equal(runtime?.attributes.description, "Runtime services");
  assert.equal(runtime?.attributes.symbol_count, 7);
});

test("edge direction distinguishes provider flow from consumer uses", () => {
  const architecture = presentGraphProjection(result("directory", [
    group("directory", "directory:consumer", "consumer"),
    group("directory", "directory:provider", "provider"),
    node("consumer/use.c", "c"),
    node("provider/api.h", "c"),
    membership("directory:consumer", "consumer/use.c"),
    membership("directory:provider", "provider/api.h"),
    relation("consumer/use.c", "provider/api.h"),
  ]), "architecture").overview;
  const dependency = architecture.edges[0];
  const consumer = architecture.nodes.find((row) =>
    row.id === dependency.source);
  const provider = architecture.nodes.find((row) =>
    row.id === dependency.target);
  assert.ok(consumer);
  assert.ok(provider);
  assert.match(consumer.label, /consumer/);
  assert.match(provider.label, /provider/);
  const dependencyPresentation = edgePresentation(architecture, dependency);
  assert.equal(dependency.source, consumer.id);
  assert.equal(dependency.target, provider.id);
  assert.match(
    dependencyPresentation.summary,
    /consumer.*relates to.*provider/,
  );

  const flowGraph = orientGraphEdges(architecture, "flow");
  const flow = flowGraph.edges[0];
  assert.equal(flow.id, dependency.id);
  assert.equal(flow.source, provider.id);
  assert.equal(flow.target, consumer.id);
  assert.equal(
    flow.attributes?.canonical_source,
    consumer.id,
  );
  assert.equal(
    flow.attributes?.canonical_target,
    provider.id,
  );
  assert.equal(flow.attributes?.presentation_direction, "flow");
  assert.deepEqual(orientGraphEdges(architecture, "uses"), architecture);
  const flowPresentation = edgePresentation(flowGraph, flow);
  assert.equal(flowPresentation.eyebrow, "dependency flow");
  assert.match(
    flowPresentation.summary,
    /provider\/? supplies consumer\/?.*consumer\/? depends on provider/,
  );
});

test("peripheral inventories collapse into expandable typed groups", () => {
  const canonical = typedRelation(
    "packages",
    "external",
    "file:src/core.c",
    "package:yyjson",
  );
  const presentation = presentGraphProjection(result("directory", [
    group("directory", "directory:src", "src"),
    item("group:inventory:package", "Packages", {
      entity_kind: "inventory",
      group_by: "inventory",
      id: "inventory:package",
      inventory_kind: "package",
      member_count: 1,
      origin: "derived",
      record_kind: "group",
    }),
    node("src/core.c", "c"),
    membership("directory:src", "src/core.c"),
    peripheral("package", "package:yyjson", "yyjson"),
    nodeMembership("inventory:package", "package:yyjson"),
    canonical,
    item("group-relation:packages", "src packages yyjson", {
      canonical_relation_keys: [canonical.key],
      entity_kind: "group_relation",
      family: "packages",
      names: [],
      record_kind: "group_relation",
      relation_count: 1,
      relation_kind: "packages",
      relation_kinds: ["external"],
      source: "directory:src",
      target: "inventory:package",
      witness_count: 1,
    }),
  ]), "overview");
  const packages = presentation.overview.nodes.find((row) =>
    row.kind === "package" && row.attributes.presentation_role === "group");
  assert.ok(packages);
  assert.equal(packages.label, "Packages");
  assert.deepEqual(packages.attributes.member_node_ids, ["package:yyjson"]);
  assert.equal(
    presentation.overview.nodes.some((row) => row.id === "package:yyjson"),
    false,
  );
  const expanded = composeArchitectureGraph(
    presentation.overview,
    presentation.files,
    new Map(),
    {
      expandedGroups: new Set([packages.id]),
      expandedFiles: new Set(),
      hidden: new Set(),
      revealedSymbols: new Set(),
    },
  );
  assert.equal(
    expanded.nodes.find((row) => row.id === "package:yyjson")?.parent,
    packages.id,
  );
  assert.ok(expanded.edges.some((row) => row.target === "package:yyjson"));
});

test("layer labels and projection frontiers are carried from typed graph records", () => {
  const architecture = presentGraphProjection(result(
    "layer",
    [
      group("layer", "layer:auto-c", "C"),
      node("src/core.c", "c"),
      membership("layer:auto-c", "src/core.c"),
    ],
    "incomplete",
  ), "architecture");
  assert.equal(architecture.grouping, "layer");
  assert.equal(architecture.overview.nodes[0].label, "C");
  assert.equal(architecture.overview.diagnostics.length, 1);
  assert.match(
    String(architecture.overview.diagnostics[0].message),
    /explicit evidence frontier/,
  );
});

test("repository coverage frontiers remain visible without degrading graph completeness", () => {
  const coverage = item("coverage:repository", "Repository discovery", {
    completeness_scope: "contextual",
    inventory_files: 3,
    record_kind: "coverage",
    selected: 2,
    unsupported_known: 1,
  });
  coverage.state = "unknown";
  coverage.message = "repository discovery contains unsupported inputs";
  const architecture = presentGraphProjection(result("directory", [
    group("directory", "directory:src", "src"),
    node("src/core.c", "c"),
    membership("directory:src", "src/core.c"),
    coverage,
  ]), "evidence");
  assert.equal(architecture.overview.diagnostics.length, 1);
  assert.equal(architecture.overview.diagnostics[0].state, "unknown");
  assert.match(
    String(architecture.overview.diagnostics[0].message),
    /unsupported inputs/,
  );
});

test("graph adapter rejects unknown relation and membership endpoints", () => {
  assert.throws(
    () => presentGraphProjection(result("directory", [
      group("directory", "directory:src", "src"),
      node("src/core.c", "c"),
      membership("directory:src", "missing.c"),
    ]), "architecture"),
    /unknown node/,
  );
  assert.throws(
    () => presentGraphProjection(result("directory", [
      group("directory", "directory:src", "src"),
      node("src/core.c", "c"),
      membership("directory:src", "src/core.c"),
      relation("src/core.c", "missing.c"),
    ]), "architecture"),
    /unknown endpoint/,
  );
});

test("Map view presets and grouping compile to one graph ProjectionPlan", () => {
  assert.deepEqual(mapGraphProjectionPlan("overview", "component"), {
    group_by: "component",
    id: "app-overview-component",
    level: "file",
    overlays: ["diagnostics", "evidence-quality"],
    relations: ["bridges", "builds", "imports", "packages", "tests"],
    select: "graph",
  });
  assert.deepEqual(
    mapGraphProjectionPlan("tests", "directory").relations,
    ["tests"],
  );
  assert.deepEqual(
    mapGraphProjectionPlan("evidence", "layer").relations,
    [],
  );
  assert.equal(
    mapGraphProjectionPlan("architecture", "language").group_by,
    "language",
  );
  for (const view of [
    "overview",
    "architecture",
    "tests",
    "evidence",
  ] as const) {
    for (const grouping of ["component", "directory", "language", "layer"] as const) {
      const { id: _appId, ...appPlan } =
        mapGraphProjectionPlan(view, grouping);
      const { id: _hostId, ...hostPlan } = {
        ...mapProjectionRequest({ groupBy: grouping, view }).definition,
      };
      assert.deepEqual(
        appPlan,
        hostPlan,
        `${view}/${grouping} app plan drifted from the host compiler`,
      );
    }
  }
});

test("ProjectionResult parsing rejects duplicate items and mismatched selectors", () => {
  const projection = result("directory", [
    group("directory", "directory:src", "src"),
    node("src/core.c", "c"),
    membership("directory:src", "src/core.c"),
  ]);
  const bytes = new TextEncoder().encode(JSON.stringify(projection));
  assert.equal(parseProjectionResult(bytes, "graph").id, "app-directory");
  assert.throws(
    () => parseProjectionResult(bytes, "file_edges"),
    /expected file_edges projection/,
  );
  projection.fact.items.push(projection.fact.items[0]);
  assert.throws(
    () => parseProjectionResult(
      new TextEncoder().encode(JSON.stringify(projection)),
      "graph",
    ),
    /duplicate item/,
  );
});
