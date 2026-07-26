import { describe, expect, it } from "vitest";
import type { GraphView } from "../src/artifacts/model";
import {
  elkHierarchy,
  elkLeafSize,
  elkLayoutOptions,
  elkPositions,
} from "../src/graph/layout";

describe("compound graph layout", () => {
  it("represents file containment in ELK and restores absolute child positions", () => {
    const nodes: GraphView["nodes"] = [
      { attributes: {}, evidence: [], id: "file", identity: "src/a.c", kind: "file", label: "a.c", parent: null },
      { attributes: {}, evidence: [], id: "symbol", identity: "src/a.c:f", kind: "symbol", label: "f", parent: "file" },
      { attributes: {}, evidence: [], id: "target", identity: "builtin:x", kind: "builtin", label: "x", parent: null },
    ];
    expect(elkHierarchy(nodes)).toEqual([
      {
        children: [{ height: 52, id: "symbol", width: 170 }],
        id: "file",
        layoutOptions: { "elk.padding": "[top=48,left=24,bottom=24,right=24]" },
      },
      { height: 52, id: "target", width: 170 },
    ]);

    expect(elkPositions([
      {
        children: [{ height: 20, id: "symbol", width: 40, x: 10, y: 15 }],
        height: 70,
        id: "file",
        width: 80,
        x: 100,
        y: 200,
      },
    ])).toEqual({
      file: { x: 140, y: 235 },
      symbol: { x: 130, y: 225 },
    });
  });

  it("scales graph and compound-node spacing without changing graph content", () => {
    expect(elkLeafSize(100)).toEqual({ height: 52, width: 170 });
    expect(elkLeafSize(60)).toEqual({ height: 31, width: 102 });
    expect(elkLeafSize(5)).toEqual({ height: 30, width: 40 });
    expect(elkLayoutOptions("LR", 60)).toMatchObject({
      "elk.direction": "RIGHT",
      "elk.layered.spacing.nodeNodeBetweenLayers": "54",
      "elk.spacing.nodeNode": "26",
    });
    expect(elkLayoutOptions("TB", 35)).toMatchObject({
      "elk.direction": "DOWN",
      "elk.layered.spacing.nodeNodeBetweenLayers": "32",
      "elk.spacing.nodeNode": "15",
    });
    expect(elkLayoutOptions("LR", 5)).toMatchObject({
      "elk.direction": "RIGHT",
      "elk.layered.spacing.nodeNodeBetweenLayers": "6",
      "elk.spacing.nodeNode": "2",
    });
    expect(elkHierarchy([
      {
        attributes: {},
        evidence: [],
        id: "file",
        identity: "a",
        kind: "file",
        label: "a",
        parent: null,
      },
      {
        attributes: {},
        evidence: [],
        id: "symbol",
        identity: "a:f",
        kind: "symbol",
        label: "f",
        parent: "file",
      },
    ], 60)[0].layoutOptions).toEqual({
      "elk.padding": "[top=29,left=14,bottom=14,right=14]",
    });
    expect(elkHierarchy([
      {
        attributes: {},
        evidence: [],
        id: "node",
        identity: "node",
        kind: "file",
        label: "node",
        parent: null,
      },
    ], 5)).toEqual([{ height: 30, id: "node", width: 40 }]);
    expect(elkHierarchy([
      {
        attributes: {},
        evidence: [],
        id: "file",
        identity: "a",
        kind: "file",
        label: "a",
        parent: null,
      },
      {
        attributes: {},
        evidence: [],
        id: "symbol",
        identity: "a:f",
        kind: "symbol",
        label: "f",
        parent: "file",
      },
    ], 5)[0].layoutOptions).toEqual({
      "elk.padding": "[top=18,left=4,bottom=4,right=4]",
    });
  });
});
