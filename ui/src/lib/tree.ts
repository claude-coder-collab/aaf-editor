import type { TreeItem } from "./rpc";

export type Loader = (id: number, offset: number, limit: number) => Promise<{ total: number; items: TreeItem[] }>;

export interface TreeNode {
  id: number;
  parent: number;
  item: TreeItem;
  depth: number;
  expanded: boolean;
  children: TreeNode[] | null;
  total: number;
}

export type Row = { kind: "node"; node: TreeNode } | { kind: "more"; parent: TreeNode | null; depth: number; remaining: number };

export const PAGE = 200;

/// Lazily loaded, paged view of the strong-reference tree.
export class TreeModel {
  roots: TreeNode[] = [];
  rootTotal = 0;
  private readonly byId = new Map<number, TreeNode>();

  constructor(
    private readonly load: Loader,
    private readonly rootId = 0,
  ) {}

  async init(): Promise<void> {
    this.byId.clear();
    const page = await this.load(this.rootId, 0, PAGE);
    this.rootTotal = page.total;
    this.roots = page.items.map((item) => this.makeNode(item, 0, this.rootId));
  }

  get(id: number): TreeNode | undefined {
    return this.byId.get(id);
  }

  rows(): Row[] {
    const out: Row[] = [];
    const walk = (nodes: TreeNode[], total: number, parent: TreeNode | null, depth: number) => {
      for (const node of nodes) {
        out.push({ kind: "node", node });
        if (node.expanded && node.children) {
          walk(node.children, node.total, node, depth + 1);
        }
      }
      if (nodes.length < total) {
        out.push({ kind: "more", parent, depth, remaining: total - nodes.length });
      }
    };
    walk(this.roots, this.rootTotal, null, 0);
    return out;
  }

  async expand(node: TreeNode): Promise<void> {
    if (node.item.childCount === 0) {
      return;
    }
    if (!node.children) {
      const page = await this.load(node.id, 0, PAGE);
      node.total = page.total;
      node.children = page.items.map((item) => this.makeNode(item, node.depth + 1, node.id));
    }
    node.expanded = true;
  }

  collapse(node: TreeNode): void {
    node.expanded = false;
  }

  async loadMore(parent: TreeNode | null): Promise<void> {
    const list = parent ? parent.children : this.roots;
    if (!list) {
      return;
    }
    const page = await this.load(parent ? parent.id : this.rootId, list.length, PAGE);
    const depth = parent ? parent.depth + 1 : 0;
    list.push(...page.items.map((item) => this.makeNode(item, depth, parent ? parent.id : this.rootId)));
    if (parent) parent.total = page.total;
    else this.rootTotal = page.total;
  }

  /// Expands every ancestor in `path` (root first, excluding the tree root) and loads pages until each is visible.
  async reveal(path: number[]): Promise<TreeNode | undefined> {
    let list = this.roots;
    let parent: TreeNode | null = null;
    let found: TreeNode | undefined;
    for (const id of path) {
      if (id === this.rootId) continue;
      found = list.find((n) => n.id === id);
      while (!found && list.length < (parent ? parent.total : this.rootTotal)) {
        await this.loadMore(parent);
        found = list.find((n) => n.id === id);
      }
      if (!found) return undefined;
      if (id !== path[path.length - 1]) {
        await this.expand(found);
        list = found.children ?? [];
        parent = found;
      }
    }
    return found;
  }

  /// Reloads the loaded children of changed objects and of their parents (whose rows show their labels),
  /// keeping expansion state where the same objects remain.
  async refresh(changed: Iterable<number>): Promise<void> {
    const targets = new Set<number>();
    for (const id of changed) {
      targets.add(id);
      const node = this.byId.get(id);
      if (node) targets.add(node.parent);
    }
    const reload = async (id: number, node: TreeNode | null) => {
      const previous = new Map((node ? (node.children ?? []) : this.roots).map((n) => [n.id, n]));
      const count = Math.max(PAGE, previous.size);
      const page = await this.load(id, 0, count);
      const depth = node ? node.depth + 1 : 0;
      const nodes = page.items.map((item) => {
        const old = previous.get(item.id);
        if (old) {
          old.item = item;
          return old;
        }
        return this.makeNode(item, depth, id);
      });
      if (node) {
        node.children = nodes;
        node.total = page.total;
        node.item = { ...node.item, childCount: page.total };
      } else {
        this.roots = nodes;
        this.rootTotal = page.total;
      }
    };
    for (const id of targets) {
      if (id === this.rootId) {
        await reload(id, null);
        continue;
      }
      const node = this.byId.get(id);
      if (node?.children) {
        await reload(id, node);
      }
    }
  }

  private makeNode(item: TreeItem, depth: number, parent: number): TreeNode {
    const node: TreeNode = { id: item.id, parent, item, depth, expanded: false, children: null, total: item.childCount };
    this.byId.set(item.id, node);
    return node;
  }
}
