/// <reference types="vite/client" />

declare module "archbird/wasm" {
  type Core = {
    mapExportGraph(
      artifact: Uint8Array,
      format: "json",
      view: "components" | "files" | "symbols",
      direction: "LR" | "RL" | "TB" | "BT",
      maxNodes: number,
      maxEdgeNames: number,
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
