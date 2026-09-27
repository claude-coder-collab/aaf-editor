import { describe, expect, it } from "vitest";

import type { TreeItem } from "./rpc";
import { PAGE, TreeModel } from "./tree";

function item(id: number, childCount = 0, label = `object ${id}`): TreeItem {
  return { id, class: "C", label, pid: 1, property: "p", index: 0, key: "", childCount };
}

function fakeLoader(children: Map<number, TreeItem[]>) {
  const calls: string[] = [];
  const load = async (id: number, offset: number, limit: number) => {
    calls.push(`${id}:${offset}`);
    const all = children.get(id) ?? [];
    return { total: all.length, items: all.slice(offset, offset + limit) };
  };
  return { load, calls };
}

describe("TreeModel", () => {
  it("loads lazily and flattens expanded nodes", async () => {
    const data = new Map([
      [0, [item(1, 2), item(2)]],
      [1, [item(3), item(4)]],
    ]);
    const { load, calls } = fakeLoader(data);
    const tree = new TreeModel(load);
    await tree.init();
    expect(tree.rows().map((r) => (r.kind === "node" ? r.node.id : "more"))).toEqual([1, 2]);
    await tree.expand(tree.get(1)!);
    expect(tree.rows().map((r) => (r.kind === "node" ? r.node.id : "more"))).toEqual([1, 3, 4, 2]);
    expect(tree.get(3)!.depth).toBe(1);
    tree.collapse(tree.get(1)!);
    await tree.expand(tree.get(1)!);
    expect(calls).toEqual(["0:0", "1:0"]);
    await tree.expand(tree.get(2)!);
    expect(tree.get(2)!.expanded).toBe(false);
  });

  it("pages large collections and reveals deep paths", async () => {
    const many = Array.from({ length: PAGE * 2 + 5 }, (_, i) => item(100 + i));
    const data = new Map<number, TreeItem[]>([
      [0, [item(1, many.length)]],
      [1, many],
    ]);
    const { load } = fakeLoader(data);
    const tree = new TreeModel(load);
    await tree.init();
    await tree.expand(tree.get(1)!);
    const more = tree.rows().at(-1)!;
    expect(more.kind === "more" && more.remaining).toBe(PAGE + 5);
    const target = many.at(-1)!.id;
    const found = await tree.reveal([0, 1, target]);
    expect(found?.id).toBe(target);
    expect(tree.rows().some((r) => r.kind === "more")).toBe(false);
  });

  it("refreshes changed objects and their parents' rows", async () => {
    const data = new Map([
      [0, [item(1, 1)]],
      [1, [item(2, 0, "old")]],
    ]);
    const { load } = fakeLoader(data);
    const tree = new TreeModel(load);
    await tree.init();
    await tree.expand(tree.get(1)!);
    data.set(1, [item(2, 0, "new"), item(5)]);
    await tree.refresh([2]);
    expect(tree.get(2)!.item.label).toBe("new");
    expect(tree.get(1)!.children!.map((n) => n.id)).toEqual([2, 5]);
    expect(tree.get(1)!.expanded).toBe(true);
  });
});
