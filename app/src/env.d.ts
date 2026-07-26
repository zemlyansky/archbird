/// <reference types="vite/client" />

declare module "archbird/wasm" {
  type Core = {
    mapDiff(
      before: Uint8Array,
      after: Uint8Array,
      pretty?: boolean,
    ): Uint8Array;
    mapExportGraph(
      artifact: Uint8Array,
      format: "graphml" | "json" | "mermaid",
      view: "components" | "files" | "symbols",
      direction: "LR" | "RL" | "TB" | "BT",
      maxNodes: number,
      maxEdgeNames: number,
    ): Uint8Array;
    mapQuery(
      map: Uint8Array,
      resolution: Uint8Array,
      query: Uint8Array,
      pretty?: boolean,
    ): Uint8Array;
    projectionEvaluate(
      map: Uint8Array,
      resolution: Uint8Array,
      projection: Uint8Array,
      pretty?: boolean,
    ): Uint8Array;
  };

  const wasm: {
    createArchbirdCore(options?: {
      wasmBinary?: ArrayBuffer | Uint8Array;
    }): Promise<Core>;
  };
  export default wasm;
}

declare module "archbird/browser" {
  const browser: {
    createBrowserArchbird(options?: {
      wasmBinary?: ArrayBuffer | Uint8Array;
    }): Promise<unknown>;
  };
  export default browser;
}
