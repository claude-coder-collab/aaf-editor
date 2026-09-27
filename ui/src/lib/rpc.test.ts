import { describe, expect, it } from "vitest";

import { RpcClient, RpcError } from "./rpc";

describe("RpcClient", () => {
  it("sends JSON-RPC 2.0 requests and unwraps results", async () => {
    const sent: unknown[] = [];
    const client = new RpcClient(async (request) => {
      const parsed = JSON.parse(request);
      sent.push(parsed);
      return { jsonrpc: "2.0", id: parsed.id, result: { open: false } };
    });
    await expect(client.docInfo()).resolves.toEqual({ open: false });
    expect(sent[0]).toEqual({ jsonrpc: "2.0", id: 1, method: "doc.info", params: {} });
  });

  it("raises RpcError for error responses, also when the transport returns text", async () => {
    const client = new RpcClient(async (request) => JSON.stringify({ jsonrpc: "2.0", id: JSON.parse(request).id, error: { code: -32000, message: "nope" } }));
    const error = await client.object(5).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(RpcError);
    expect((error as RpcError).code).toBe(-32000);
  });

  it("dispatches events to subscribers until they unsubscribe", () => {
    const client = new RpcClient(async () => ({}));
    const seen: string[] = [];
    const off = client.onEvent((method) => seen.push(method));
    client.dispatch("doc.changed", {});
    off();
    client.dispatch("doc.changed", {});
    expect(seen).toEqual(["doc.changed"]);
  });
});
