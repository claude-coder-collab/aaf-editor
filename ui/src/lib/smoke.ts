import type { RpcClient } from "./rpc";

/// Exercises the host bridge end to end: open, browse, edit, undo, validate. Returns a process exit code.
export async function runSmokeTest(client: RpcClient, file: string): Promise<{ code: number; message: string }> {
  try {
    const info = await client.open(file);
    if (!info.open || info.header === null || info.header === undefined) throw new Error("document did not open");
    const children = await client.children(0);
    if (children.total < 1) throw new Error("root has no children");
    const mobs = await client.search("", "Mob", 50);
    let mob: (typeof mobs)[number] | undefined;
    let name: { pid: number } | undefined;
    for (const candidate of mobs) {
      const object = await client.object(candidate.id);
      name = object.properties.find((p) => p.name === "Name");
      if (name) {
        mob = candidate;
        break;
      }
    }
    if (!mob || !name) throw new Error("no named mob found");
    await client.setProperty(mob.id, name.pid, { t: "string", v: "Smoke test" });
    const renamed = await client.object(mob.id);
    if (renamed.label !== "Smoke test") throw new Error("rename was not applied");
    await client.undo();
    const diagnostics = await client.validate();
    const errors = diagnostics.filter((d) => d.severity === "error");
    if (errors.length > 0) throw new Error(`validation errors: ${errors[0]?.message}`);
    return { code: 0, message: "smoke test passed" };
  } catch (e) {
    return { code: 1, message: `smoke test failed: ${e instanceof Error ? e.message : String(e)}` };
  }
}
