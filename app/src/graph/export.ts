import type { GraphView } from "../artifacts/model";
import type { GraphDirection } from "../adapters/saved-artifact";

const encoder = new TextEncoder();

function xml(value: unknown): string {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&apos;");
}

function graphml(graph: GraphView): string {
  const lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<graphml xmlns="http://graphml.graphdrawing.org/xmlns">',
    '  <key id="label" for="all" attr.name="label" attr.type="string"/>',
    '  <key id="kind" for="all" attr.name="kind" attr.type="string"/>',
    '  <key id="identity" for="node" attr.name="identity" attr.type="string"/>',
    '  <key id="parent" for="node" attr.name="parent" attr.type="string"/>',
    '  <key id="classification" for="edge" attr.name="classification" attr.type="string"/>',
    '  <key id="names" for="edge" attr.name="names" attr.type="string"/>',
    `  <graph id="${xml(graph.project)}" edgedefault="directed">`,
  ];
  for (const node of graph.nodes) {
    lines.push(
      `    <node id="${xml(node.id)}">`,
      `      <data key="label">${xml(node.label)}</data>`,
      `      <data key="kind">${xml(node.kind)}</data>`,
      `      <data key="identity">${xml(node.identity)}</data>`,
      `      <data key="parent">${xml(node.parent || "")}</data>`,
      "    </node>",
    );
  }
  for (const edge of graph.edges) {
    lines.push(
      `    <edge id="${xml(edge.id)}" source="${xml(edge.source)}" target="${xml(edge.target)}">`,
      `      <data key="label">${xml(edge.kind)}</data>`,
      `      <data key="kind">${xml(edge.kind)}</data>`,
      `      <data key="classification">${xml(edge.classification)}</data>`,
      `      <data key="names">${xml(edge.names.join(", "))}</data>`,
      "    </edge>",
    );
  }
  lines.push("  </graph>", "</graphml>", "");
  return lines.join("\n");
}

function mermaidLabel(value: string): string {
  return value.replaceAll("\\", "\\\\").replaceAll('"', '\\"').replaceAll("\n", " ");
}

function mermaid(graph: GraphView, direction: GraphDirection): string {
  const ids = new Map(graph.nodes.map((node, index) => [node.id, `n${index}`]));
  const lines = [
    "%% Archbird projected architecture view",
    `flowchart ${direction}`,
  ];
  for (const node of graph.nodes) {
    lines.push(`  ${ids.get(node.id)}["${mermaidLabel(node.label)}"]`);
  }
  for (const edge of graph.edges) {
    const source = ids.get(edge.source);
    const target = ids.get(edge.target);
    if (!source || !target) continue;
    const names = edge.names.length ? ` · ${edge.names.join(", ")}` : "";
    lines.push(
      `  ${source} -->|"${mermaidLabel(`${edge.kind}${names}`)}"| ${target}`,
    );
  }
  lines.push("");
  return lines.join("\n");
}

export function exportPresentedGraph(
  graph: GraphView,
  format: "graphml" | "mermaid",
  direction: GraphDirection,
): Uint8Array {
  return encoder.encode(format === "graphml"
    ? graphml(graph)
    : mermaid(graph, direction));
}
