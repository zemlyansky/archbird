// @vitest-environment happy-dom

import { afterEach, describe, expect, it, vi } from "vitest";

import { ServerHost } from "../src/adapters/server-host";

const artifactBytes = new TextEncoder().encode("{}");
const artifactDigest = "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a";

class TestEventSource {
  addEventListener(): void {}
  close(): void {}
}

afterEach(() => {
  vi.unstubAllGlobals();
  document.head.replaceChildren();
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
        return Response.json({ protocol_version: 1 });
      }
      if (url.endsWith("api/v1/request")) {
        const request = JSON.parse(String(init?.body));
        return Response.json({
          id: request.id,
          ok: true,
          protocol_version: 1,
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
