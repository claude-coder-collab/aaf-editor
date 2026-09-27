<script lang="ts">
  import type { Row, TreeModel, TreeNode } from "../tree";

  interface Props {
    tree: TreeModel;
    version: number;
    selected: number | null;
    onselect: (id: number) => void;
    onchanged: () => void;
  }

  let { tree, version, selected, onselect, onchanged }: Props = $props();

  const ROW = 24;
  let scrollTop = $state(0);
  let height = $state(400);
  let container: HTMLDivElement | undefined = $state();

  const rows = $derived.by(() => {
    void version;
    return tree.rows();
  });
  const first = $derived(Math.max(0, Math.floor(scrollTop / ROW) - 10));
  const last = $derived(Math.min(rows.length, Math.ceil((scrollTop + height) / ROW) + 10));
  const visible = $derived(rows.slice(first, last));

  async function toggle(node: TreeNode) {
    if (node.expanded) {
      tree.collapse(node);
    } else {
      await tree.expand(node);
    }
    onchanged();
  }

  async function more(row: Extract<Row, { kind: "more" }>) {
    await tree.loadMore(row.parent);
    onchanged();
  }

  function indexOfSelected(): number {
    return rows.findIndex((r) => r.kind === "node" && r.node.id === selected);
  }

  export function scrollToSelected() {
    const index = indexOfSelected();
    if (index < 0 || !container) return;
    const top = index * ROW;
    if (top < container.scrollTop || top + ROW > container.scrollTop + container.clientHeight) {
      container.scrollTop = Math.max(0, top - container.clientHeight / 2);
    }
  }

  async function keydown(event: KeyboardEvent) {
    const index = indexOfSelected();
    const row = rows[index];
    const node = row?.kind === "node" ? row.node : undefined;
    const pick = (i: number) => {
      const r = rows[Math.max(0, Math.min(rows.length - 1, i))];
      if (r?.kind === "node") {
        onselect(r.node.id);
        queueMicrotask(scrollToSelected);
      }
    };
    switch (event.key) {
      case "ArrowDown":
        pick(index + 1);
        break;
      case "ArrowUp":
        pick(index - 1);
        break;
      case "ArrowRight":
        if (node && !node.expanded && node.item.childCount > 0) await toggle(node);
        else pick(index + 1);
        break;
      case "ArrowLeft":
        if (node?.expanded) await toggle(node);
        else if (node) {
          const parent = rows.findIndex((r) => r.kind === "node" && r.node.id === node.parent);
          if (parent >= 0) pick(parent);
        }
        break;
      case "Enter":
        if (node) await toggle(node);
        break;
      default:
        return;
    }
    event.preventDefault();
  }
</script>

<div class="tree" bind:this={container} bind:clientHeight={height} onscroll={(e) => (scrollTop = (e.currentTarget as HTMLDivElement).scrollTop)} tabindex="0" role="tree" onkeydown={keydown}>
  <div class="spacer" style:height="{rows.length * ROW}px">
    {#each visible as row, i (first + i)}
      <div class="row" style:top="{(first + i) * ROW}px">
        {#if row.kind === "node"}
          {@const node = row.node}
          <div
            class="item"
            class:selected={node.id === selected}
            style:padding-left="{node.depth * 14 + 4}px"
            role="treeitem"
            aria-selected={node.id === selected}
            tabindex="-1"
            onclick={() => onselect(node.id)}
            ondblclick={() => toggle(node)}
            onkeydown={() => {}}
          >
            <button class="expander" class:hidden={node.item.childCount === 0} onclick={(e) => (e.stopPropagation(), toggle(node))} tabindex="-1">
              {node.expanded ? "▾" : "▸"}
            </button>
            <span class="label">{node.item.label}</span>
            <span class="class">{node.item.class !== node.item.label ? node.item.class : ""}</span>
            {#if node.item.childCount > 0}<span class="count">{node.item.childCount}</span>{/if}
          </div>
        {:else}
          <button class="more" style:margin-left="{row.depth * 14 + 22}px" onclick={() => more(row)}>Load {Math.min(row.remaining, 200)} more of {row.remaining}…</button>
        {/if}
      </div>
    {/each}
  </div>
</div>

<style>
  .tree {
    height: 100%;
    overflow: auto;
    position: relative;
    outline: none;
    font-size: 13px;
  }
  .tree:focus-visible {
    box-shadow: inset 0 0 0 2px var(--accent);
  }
  .spacer {
    position: relative;
  }
  .row {
    position: absolute;
    left: 0;
    right: 0;
    height: 24px;
  }
  .item {
    display: flex;
    align-items: center;
    gap: 6px;
    height: 24px;
    cursor: default;
    white-space: nowrap;
    padding-right: 8px;
  }
  .item:hover {
    background: var(--hover);
  }
  .item.selected {
    background: var(--selection);
  }
  .expander {
    width: 16px;
    border: none;
    background: none;
    color: var(--muted);
    padding: 0;
    cursor: pointer;
    font-size: 11px;
  }
  .expander.hidden {
    visibility: hidden;
  }
  .label {
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .class {
    color: var(--muted);
    font-size: 11px;
  }
  .count {
    margin-left: auto;
    color: var(--muted);
    font-size: 11px;
  }
  .more {
    border: none;
    background: none;
    color: var(--accent);
    cursor: pointer;
    font-size: 12px;
    height: 24px;
    padding: 0;
  }
</style>
