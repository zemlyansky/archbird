// @vitest-environment happy-dom

import { afterEach, describe, expect, it, vi } from "vitest";

import { ServerHost } from "../src/adapters/server-host";

const artifactBytes = new TextEncoder().encode("{}");
const artifactDigest = "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";

class TestEventSource {
  static latest: TestEventSource | null = null;
  private listeners = new Map<string, (event: MessageEvent<string>) => void>();

  constructor() {
    TestEventSource.latest = this;
  }

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    this.listeners.set(type, listener as (event: MessageEvent<string>) => void);
  }

  emit(type: string, value: unknown): void {
    this.listeners.get(type)?.({ data: JSON.stringify(value) } as MessageEvent<string>);
  }

  close(): void {}
}

afterEach(() => {
  vi.unstubAllGlobals();
  document.head.replaceChildren();
  TestEventSource.latest = null;
});

describe("server artifact transport", () => {
  it("validates artifacts when Web Crypto is unavailable on plain LAN HTTP", async () => {
    const marker = document.createElement("meta");
    marker.name = "archbird-host";
    marker.content = "server";
    document.head.append(marker);
    vi.stubGlobal("EventSource", TestEventSource);
    vi.stubGlobal("crypto", {
      getRandomValues(value: Uint8Array) {
        value.fill(1);
        return value;
      },
    });
    const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (url.endsWith("api/v1/bootstrap")) {
        return Response.json({ protocol_version: 2 });
      }
      if (url.endsWith("api/v1/request")) {
        const request = JSON.parse(String(init?.body));
        return Response.json({
          ok: true,
          protocol_version: 2,
          result: {
            blob_sha256: artifactDigest,
            bytes: artifactBytes.byteLength,
            generation: "1".repeat(64),
            project: "fixture",
          },
          session: request.session,
        });
      }
      if (url.endsWith(`api/v1/blobs/${artifactDigest}`)) {
        return new Response(artifactBytes, {
          headers: { "X-Content-SHA256": artifactDigest },
        });
      }
      throw new Error(`unexpected request: ${url}`);
    });
    vi.stubGlobal("fetch", fetchMock);

    const host = await ServerHost.connect();
    expect(host).not.toBeNull();
    const artifact = await host!.map();
    const observedDisplay = {
      artifact: "map",
      expanded: { files: [], groups: [], omitted: 0, symbols: [] },
      generation: "1".repeat(64),
      graph: null,
      hidden: { ids: [], omitted: 0 },
      history_depth: 0,
      selected: null,
      selected_constraint: null,
      source: null,
      workspace: "live" as const,
    };
    await host!.observeDisplay(observedDisplay);

    const requestCall = fetchMock.mock.calls.find(([input, init]) =>
      String(input).endsWith("api/v1/request")
      && JSON.parse(String(init?.body)).method === "map");
    const request = JSON.parse(String(requestCall?.[1]?.body));
    expect(request).toEqual({ method: "map", params: {}, session: expect.any(String) });
    const stateCall = fetchMock.mock.calls.find(([input, init]) =>
      String(input).endsWith("api/v1/request")
      && JSON.parse(String(init?.body)).method === "state");
    expect(JSON.parse(String(stateCall?.[1]?.body))).toEqual({
      method: "state",
      params: { observed_display: observedDisplay },
      session: request.session,
    });
    const listener = vi.fn();
    host!.subscribe(listener);
    TestEventSource.latest!.emit("display-updated", {
      event_id: 1,
      generation: "1".repeat(64),
      payload: {
        artifact: {
          blob_sha256: artifactDigest,
          bytes: artifactBytes.byteLength,
          generation: "1".repeat(64),
          project: "fixture",
        },
        display: {},
        generation: "1".repeat(64),
        method: "map",
        params: {},
        revision: 1,
      },
      protocol_version: 2,
      type: "display-updated",
    });
    expect(listener).toHaveBeenCalledWith(expect.objectContaining({
      type: "display-updated",
    }));

    expect(new Uint8Array(artifact.bytes)).toEqual(artifactBytes);
    expect(fetchMock).toHaveBeenCalledWith(
      expect.stringContaining(`/api/v1/blobs/${artifactDigest}`),
      expect.objectContaining({
        integrity: "sha256-RBNvo1WzZ4oRRq0W9+hknpT7T8If536DEMBg9hyq/4o=",
      }),
    );
    await host!.dispose();
  });
});
