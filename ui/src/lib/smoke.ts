import type { RpcClient } from "./rpc";

/// Exercises the host bridge end to end: open by a (simulated) drop, browse, edit, undo, validate. Returns a process exit code.
export async function runSmokeTest(client: RpcClient, file: string, drop: (file: string) => Promise<unknown>): Promise<{ code: number; message: string }> {
  try {
    await drop(file);
    const fileName = file.split(/[\\/]/).pop();
    let info = await client.docInfo();
    for (let i = 0; i < 100 && !(info.open && info.name === fileName); i++) {
      await new Promise((resolve) => setTimeout(resolve, 100));
      info = await client.docInfo();
    }
    if (!info.open || info.name !== fileName) throw new Error("dropping the file did not open it");
    if (info.header === null || info.header === undefined) throw new Error("document did not open");
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
