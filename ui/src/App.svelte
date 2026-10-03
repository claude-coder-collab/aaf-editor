<script lang="ts">
  import { onMount } from "svelte";

  import BottomPanel from "./lib/components/BottomPanel.svelte";
  import PropertyPanel from "./lib/components/PropertyPanel.svelte";
  import TimelineView from "./lib/components/TimelineView.svelte";
  import TreeView from "./lib/components/TreeView.svelte";
  import { hostCommand, hostTransport, RpcClient, type ChangeSet, type Diagnostic, type DocInfo, type MobSummary, type ObjectInfo, type SearchHit } from "./lib/rpc";
  import { runSmokeTest } from "./lib/smoke";
  import { TreeModel } from "./lib/tree";

  const transport = hostTransport();
  const client = transport ? new RpcClient(transport) : null;

  let info = $state<DocInfo>({ open: false });
  let tree = $state<TreeModel | null>(null);
  let treeVersion = $state(0);
  let selected = $state<number | null>(null);
  let object = $state<ObjectInfo | null>(null);
  let diagnostics = $state<Diagnostic[] | null>(null);
  let history = $state<{ items: string[]; position: number }>({ items: [], position: 0 });
  let message = $state<{ text: string; kind: "error" | "info" } | null>(null);
  let query = $state("");
  let results = $state<SearchHit[]>([]);
  let busy = $state(false);
  let treeView: TreeView | undefined = $state();
  let mobs = $state<MobSummary[]>([]);
  let timelines = $state<number[]>([]);
  let activeTimeline = $state<number | null>(null);

  function notify(text: string, kind: "error" | "info" = "error") {
    message = { text, kind };
    if (kind === "info") setTimeout(() => message?.text === text && (message = null), 3000);
  }

  async function guard(action: () => Promise<unknown>) {
    try {
      busy = true;
      await action();
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e));
    } finally {
      busy = false;
    }
  }

  async function reloadDocument(next: DocInfo) {
    info = next;
    diagnostics = null;
    selected = null;
    object = null;
    results = [];
    if (!client || !next.open) {
      tree = null;
      return;
    }
    const model = new TreeModel((id, offset, limit) => client.children(id, offset, limit));
    await model.init();
    tree = model;
    treeVersion++;
    history = await client.history();
    mobs = await client.mobs();
    timelines = [];
    activeTimeline = null;
    const first = mobs.find((m) => m.kind === "composition" && m.topLevel) ?? mobs.find((m) => m.kind === "composition");
    if (first) openTimeline(first.id);
    if (next.header !== null && next.header !== undefined) await select(next.header, true);
    await hostCommand("setTitle", { title: `${next.name ?? "AAF Editor"} — AAF Editor` });
  }

  async function select(id: number, reveal = false) {
    if (!client) return;
    selected = id;
    object = await client.object(id);
    if (reveal && tree) {
      await tree.reveal(await client.path(id));
      treeVersion++;
      queueMicrotask(() => treeView?.scrollToSelected());
    }
  }

  function openTimeline(mob: number) {
    if (!timelines.includes(mob)) timelines = [...timelines, mob];
    activeTimeline = mob;
  }

  function closeTimeline(mob: number) {
    timelines = timelines.filter((t) => t !== mob);
    if (activeTimeline === mob) activeTimeline = timelines.at(-1) ?? null;
  }

  const mobName = (id: number) => mobs.find((m) => m.id === id)?.name || `Mob ${id}`;

  async function applyChanges(changes: ChangeSet, mobsChanged: boolean) {
    if (!client) return;
    if (mobsChanged) {
      mobs = await client.mobs();
      timelines = timelines.filter((t) => mobs.some((m) => m.id === t));
    }
    if (tree) {
      await tree.refresh(changes.objects);
      treeVersion++;
    }
    history = await client.history();
    if (selected !== null) {
      const current = await client.object(selected);
      if (current.attached || current.id === 0) {
        object = current;
      } else {
        selected = null;
        object = null;
      }
    }
  }

  async function open(path?: string) {
    if (!client) return;
    const target = path ?? (await hostCommand<string | null>("openDialog"));
    if (!target) return;
    if (info.open && info.dirty && !confirm(`${info.name} has unsaved changes. Discard them and open ${fileName(target)}?`)) return;
    await guard(async () => reloadDocument(await client.open(target)));
  }

  const fileName = (path: string) => path.split(/[\\/]/).pop() ?? path;

  let dragging = $state(false);
  let dragTimer: ReturnType<typeof setTimeout> | undefined;

  /// WebView2 (Windows) opens a dropped file by navigating to it, which the host intercepts, so the page must leave
  /// the drop alone there. WebKit (macOS, Linux) only lets the host see the drop if the page accepts it.
  const nativeNavigationDrop = typeof (window as { chrome?: { webview?: unknown } }).chrome?.webview !== "undefined";

  /// Shows the drop hint while files are dragged over the window; the host reads the dropped file natively and calls
  /// window.__aafOpenFile.
  function dragOver(event: DragEvent) {
    if (!event.dataTransfer?.types.includes("Files")) return;
    if (!nativeNavigationDrop) {
      event.preventDefault();
      event.dataTransfer.dropEffect = "copy";
    }
    dragging = true;
    clearTimeout(dragTimer);
    dragTimer = setTimeout(() => (dragging = false), 600);
  }

  function dropped(event: DragEvent) {
    if (!event.dataTransfer?.types.includes("Files")) return;
    if (!nativeNavigationDrop) event.preventDefault();
    dragging = false;
  }

  async function save(as = false) {
    if (!client || !info.open) return;
    await guard(async () => {
      if (as) {
        const target = await hostCommand<string | null>("saveDialog", { suggested: info.path });
        if (!target) return;
        info = await client.saveAs(target);
      } else {
        info = await client.save();
      }
      notify(`Saved ${info.name}`, "info");
      await hostCommand("setTitle", { title: `${info.name} — AAF Editor` });
    });
  }

  const undo = () => client && info.canUndo && guard(() => client.undo());
  const redo = () => client && info.canRedo && guard(() => client.redo());
  const validate = () => client && info.open && guard(async () => (diagnostics = await client.validate()));

  async function goto(position: number) {
    if (!client) return;
    await guard(async () => {
      while (history.position > position) {
        await client.undo();
        history = await client.history();
      }
      while (history.position < position) {
        await client.redo();
        history = await client.history();
      }
    });
  }

  async function search() {
    if (!client || !info.open) return;
    await guard(async () => {
      results = query.trim() ? await client.search(query.trim(), "", 100) : [];
    });
  }

  async function essence(id: number, action: "extract" | "replace") {
    if (!client) return;
    await guard(async () => {
      if (action === "extract") {
        const target = await hostCommand<string | null>("saveDialog", { suggested: "essence.bin", data: true });
        if (target) notify(`Extracted ${(await client.extractEssence(id, target)).size.toLocaleString()} bytes`, "info");
      } else {
        const source = await hostCommand<string | null>("openDialog", { data: true });
        if (source) await client.replaceEssence(id, source);
      }
    });
  }

  function keydown(event: KeyboardEvent) {
    const mod = event.ctrlKey || event.metaKey;
    if (!mod) return;
    const key = event.key.toLowerCase();
    const inText = event.target instanceof HTMLInputElement || event.target instanceof HTMLTextAreaElement;
    if (key === "o") void open();
    else if (key === "s") void save(event.shiftKey);
    else if (key === "z" && !inText) void (event.shiftKey ? redo() : undo());
    else if (key === "y" && !inText) void redo();
    else if (key === "f") document.getElementById("search")?.focus();
    else return;
    event.preventDefault();
  }

  onMount(() => {
    if (!client) return;
    const off = client.onEvent((method, params) => {
      if (method === "doc.changed") {
        const p = params as { changes: ChangeSet; info: DocInfo; mobsChanged?: boolean };
        info = p.info;
        void applyChanges(p.changes, p.mobsChanged ?? true);
      } else if (method === "doc.state") {
        info = params as DocInfo;
      } else if (method === "doc.opened") {
        info = params as DocInfo;
      }
    });
    window.__aafEvent = (event) => client.dispatch(event.method, event.params);
    window.__aafOpenFile = (path) => {
      dragging = false;
      void open(path);
    };
    void (async () => {
      const current = await client.docInfo();
      if (current.open) await reloadDocument(current);
      if (window.__aafSmoke) {
        const result = await runSmokeTest(client, window.__aafSmoke.file, (file) => hostCommand("simulateDrop", { path: file }));
        await hostCommand("quit", result);
      }
    })();
    return off;
  });
</script>

<svelte:window onkeydown={keydown} ondragenter={dragOver} ondragover={dragOver} ondrop={dropped} />

{#if dragging}
  <div class="drop-hint" aria-hidden="true"><span>Drop an AAF file to open it</span></div>
{/if}

<div class="app">
  <header class="toolbar">
    <button onclick={() => open()} title="Open (Ctrl+O)">Open…</button>
    <button onclick={() => save()} disabled={!info.open} title="Save (Ctrl+S)">Save</button>
    <button onclick={() => save(true)} disabled={!info.open} title="Save as (Ctrl+Shift+S)">Save As…</button>
    <span class="sep"></span>
    <button onclick={undo} disabled={!info.canUndo} title={info.undo ? `Undo ${info.undo} (Ctrl+Z)` : "Undo"}>Undo</button>
    <button onclick={redo} disabled={!info.canRedo} title={info.redo ? `Redo ${info.redo} (Ctrl+Shift+Z)` : "Redo"}>Redo</button>
    <span class="sep"></span>
    <select class="mobs" disabled={!info.open || mobs.length === 0} value="" onchange={(e) => { const v = (e.currentTarget as HTMLSelectElement).value; if (v) openTimeline(Number(v)); (e.currentTarget as HTMLSelectElement).value = ""; }}>
      <option value="">Timelines…</option>
      {#each mobs as m (m.id)}
        <option value={String(m.id)}>{m.topLevel ? "★ " : ""}{m.name || "(unnamed)"} — {m.kind}</option>
      {/each}
    </select>
    <form class="search" onsubmit={(e) => (e.preventDefault(), search())}>
      <input id="search" type="search" placeholder="Search names and IDs (Ctrl+F)" bind:value={query} disabled={!info.open} />
    </form>
    <span class="file">
      {#if info.open}
        {info.name}{#if info.dirty}<span class="dirty" title="Unsaved changes">●</span>{/if}
        <span class="muted">· {info.objectCount?.toLocaleString()} objects · v{info.version}</span>
      {:else}
        <span class="muted">No file open</span>
      {/if}
      {#if busy}<span class="muted">working…</span>{/if}
    </span>
  </header>

  {#if message}
    <div class="message {message.kind}" role="alert">
      <span>{message.text}</span>
      <button class="icon" onclick={() => (message = null)}>✕</button>
    </div>
  {/if}

  {#if !client}
    <main class="empty">
      <p>This page is the AAF Editor's user interface and needs to run inside the editor application.</p>
    </main>
  {:else if !info.open || !tree}
    <main class="empty">
      <p>Open an AAF file to start.</p>
      <button onclick={() => open()}>Open…</button>
    </main>
  {:else}
    <main class="workspace">
      <aside class="left">
        {#if results.length > 0}
          <div class="results">
            <div class="results-head">
              <span>{results.length} matches</span>
              <button class="icon" onclick={() => (results = [])}>✕</button>
            </div>
            {#each results as hit (hit.id)}
              <button class="result" class:selected={hit.id === selected} onclick={() => select(hit.id, true)}>
                {hit.label} <span class="muted">{hit.class}</span>
              </button>
            {/each}
          </div>
        {/if}
        <TreeView bind:this={treeView} {tree} version={treeVersion} {selected} onselect={(id) => select(id)} onchanged={() => treeVersion++} />
      </aside>
      <section class="right" class:split={activeTimeline !== null}>
        {#if activeTimeline !== null}
          <div class="timelines">
            <nav class="tabs">
              {#each timelines as t (t)}
                <span class="tab" class:active={t === activeTimeline}>
                  <button class="link" onclick={() => (activeTimeline = t)}>{mobName(t)}</button>
                  <button class="icon" title="Close" onclick={() => closeTimeline(t)}>✕</button>
                </span>
              {/each}
            </nav>
            <TimelineView {client} mob={activeTimeline} {selected} {mobs} onselect={(id) => select(id, true)} onerror={(m) => notify(m, m.startsWith("Relinked") ? "info" : "error")} onwarning={(m) => notify(m, "info")} />
          </div>
        {/if}
        <div class="properties">
          {#if object}
            <PropertyPanel {client} {object} onselect={(id) => select(id, true)} onerror={(m) => notify(m)} onessence={essence} ontimeline={openTimeline} />
          {:else}
            <p class="empty">Select an object.</p>
          {/if}
        </div>
      </section>
      <footer class="bottom">
        <BottomPanel {diagnostics} {history} onselect={(id) => select(id, true)} onvalidate={validate} ongoto={goto} />
      </footer>
    </main>
  {/if}
</div>

<style>
  :global(:root) {
    --bg: #ffffff;
    --panel: #f6f7f9;
    --text: #1d2126;
    --muted: #6b7280;
    --border: #d9dde3;
    --hover: #eef1f5;
    --selection: #dbe7ff;
    --accent: #2f6fe4;
    --danger: #c0362c;
    --warn: #9a6400;
    --warn-bg: #fff4dc;
    --ok: #1f7a3a;
    --mono: ui-monospace, "SF Mono", Menlo, Consolas, monospace;
    color-scheme: light;
  }
  @media (prefers-color-scheme: dark) {
    :global(:root) {
      --bg: #17191c;
      --panel: #1f2226;
      --text: #e3e6ea;
      --muted: #9098a3;
      --border: #33383f;
      --hover: #262a30;
      --selection: #22385f;
      --accent: #6d9dff;
      --danger: #ff7b6e;
      --warn: #f0b44c;
      --warn-bg: #3a2f1a;
      --ok: #62c883;
      color-scheme: dark;
    }
  }
  :global(html, body) {
    margin: 0;
    height: 100%;
    background: var(--bg);
    color: var(--text);
    font: 13px/1.4 system-ui, -apple-system, "Segoe UI", sans-serif;
  }
  :global(#app) {
    height: 100%;
  }
  :global(button) {
    font: inherit;
    color: var(--text);
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 3px 10px;
    cursor: pointer;
  }
  :global(button:disabled) {
    opacity: 0.45;
    cursor: default;
  }
  :global(button.small) {
    padding: 1px 8px;
    font-size: 12px;
  }
  :global(button.icon) {
    padding: 0 6px;
    border: none;
    background: none;
    color: var(--muted);
  }
  :global(button.danger) {
    color: var(--danger);
  }
  :global(button.link) {
    border: none;
    background: none;
    color: var(--accent);
    padding: 0;
    text-align: left;
  }
  :global(input, select) {
    font: inherit;
    color: var(--text);
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 2px 6px;
  }
  .app {
    display: flex;
    flex-direction: column;
    height: 100%;
  }
  .toolbar {
    display: flex;
    gap: 6px;
    align-items: center;
    padding: 6px 8px;
    border-bottom: 1px solid var(--border);
    background: var(--panel);
    flex-wrap: wrap;
  }
  .sep {
    width: 1px;
    height: 20px;
    background: var(--border);
  }
  .search input {
    width: 260px;
  }
  .file {
    margin-left: auto;
    white-space: nowrap;
  }
  .dirty {
    color: var(--accent);
    margin-left: 4px;
  }
  .muted {
    color: var(--muted);
  }
  .message {
    display: flex;
    justify-content: space-between;
    padding: 6px 10px;
    font-size: 12px;
  }
  .message.error {
    background: var(--warn-bg);
    color: var(--danger);
  }
  .message.info {
    background: var(--selection);
  }
  .empty {
    padding: 32px;
    color: var(--muted);
  }
  .workspace {
    flex: 1;
    min-height: 0;
    display: grid;
    grid-template-columns: minmax(260px, 34%) 1fr;
    grid-template-rows: 1fr 190px;
  }
  .left {
    border-right: 1px solid var(--border);
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
  .left :global(.tree) {
    flex: 1;
  }
  .results {
    max-height: 40%;
    overflow: auto;
    border-bottom: 1px solid var(--border);
    display: flex;
    flex-direction: column;
  }
  .results-head {
    display: flex;
    justify-content: space-between;
    padding: 4px 8px;
    color: var(--muted);
    font-size: 12px;
  }
  .result {
    border: none;
    background: none;
    text-align: left;
    padding: 2px 10px;
    border-radius: 0;
  }
  .result.selected {
    background: var(--selection);
  }
  .right {
    min-height: 0;
    min-width: 0;
    display: grid;
    grid-template-rows: 1fr;
  }
  .right.split {
    grid-template-rows: minmax(180px, 48%) 1fr;
  }
  .timelines {
    display: flex;
    flex-direction: column;
    min-height: 0;
    border-bottom: 1px solid var(--border);
  }
  .tabs {
    display: flex;
    gap: 2px;
    padding: 2px 6px 0;
    border-bottom: 1px solid var(--border);
    overflow-x: auto;
  }
  .tab {
    display: flex;
    align-items: center;
    gap: 2px;
    padding: 2px 4px 2px 8px;
    border-radius: 4px 4px 0 0;
    font-size: 12px;
    white-space: nowrap;
  }
  .tab.active {
    background: var(--panel);
    font-weight: 600;
  }
  .properties {
    min-height: 0;
    overflow: hidden;
  }
  .mobs {
    max-width: 220px;
  }
  .bottom {
    grid-column: 1 / -1;
    min-height: 0;
  }
  @media (max-width: 700px) {
    .workspace {
      grid-template-columns: 1fr;
      grid-template-rows: 40% 1fr 160px;
    }
    .left {
      border-right: none;
      border-bottom: 1px solid var(--border);
    }
  }
  .drop-hint {
    position: fixed;
    inset: 8px;
    border: 2px dashed var(--accent);
    border-radius: 10px;
    background: color-mix(in srgb, var(--bg) 70%, transparent);
    display: flex;
    align-items: center;
    justify-content: center;
    pointer-events: none;
    z-index: 100;
  }
  .drop-hint span {
    font-size: 18px;
    padding: 10px 18px;
    border-radius: 8px;
    background: var(--panel);
    color: var(--text);
  }
</style>
