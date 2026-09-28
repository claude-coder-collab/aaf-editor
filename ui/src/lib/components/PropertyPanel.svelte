<script lang="ts">
  import type { AvailableProperty, ObjectInfo, PropertyInfo, RpcClient, SearchHit, TaggedValue } from "../rpc";
  import { defaultValue, formatValue } from "../values";
  import ValueEditor from "./ValueEditor.svelte";

  interface Props {
    client: RpcClient;
    object: ObjectInfo;
    onselect: (id: number) => void;
    onerror: (message: string) => void;
    onessence: (id: number, action: "extract" | "replace") => void;
    ontimeline: (mob: number) => void;
  }

  let { client, object, onselect, onerror, onessence, ontimeline }: Props = $props();

  let candidates = $state<Record<number, SearchHit[]>>({});
  let addPid = $state<number | null>(null);
  let createFor = $state<{ pid: number; classes: { name: string; id: string }[] } | null>(null);

  $effect(() => {
    void object.id;
    candidates = {};
    addPid = null;
    createFor = null;
  });

  async function attempt(action: () => Promise<unknown>) {
    try {
      await action();
    } catch (e) {
      onerror(e instanceof Error ? e.message : String(e));
    }
  }

  const setValue = (p: PropertyInfo, value: TaggedValue) => attempt(() => client.setProperty(object.id, p.pid, value));
  const removeProperty = (p: PropertyInfo) => attempt(() => client.removeProperty(object.id, p.pid));

  async function loadCandidates(p: PropertyInfo) {
    if (candidates[p.pid]) return;
    await attempt(async () => {
      candidates = { ...candidates, [p.pid]: await client.candidates(object.id, p.pid) };
    });
  }

  const setWeak = (p: PropertyInfo, target: string) => attempt(() => client.setWeakRef(object.id, p.pid, Number(target)));

  async function beginCreate(pid: number, typeId: string | undefined) {
    const type = typeId ? object.types[typeId] : undefined;
    const elementType = type?.kind === "strong_ref" ? type : type?.element ? object.types[type.element] : undefined;
    const className = elementType?.className ?? type?.className;
    if (!className) {
      onerror("Cannot determine the class of new children");
      return;
    }
    await attempt(async () => {
      const classes = await client.subclasses(className);
      if (classes.length === 1) {
        await client.create(object.id, pid, classes[0]!.name);
      } else {
        createFor = { pid, classes };
      }
    });
  }

  async function create(cls: string) {
    const target = createFor;
    createFor = null;
    if (target) await attempt(() => client.create(object.id, target.pid, cls));
  }

  async function addProperty() {
    const available = object.available.find((a) => a.pid === addPid);
    addPid = null;
    if (!available) return;
    await addAvailable(available);
  }

  async function addAvailable(available: AvailableProperty) {
    if (available.kind === "data") {
      const value = defaultValue(object.types, available.type);
      if (!value) {
        onerror(`No default value for ${available.name}`);
        return;
      }
      await attempt(() => client.setProperty(object.id, available.pid, value));
    } else if (available.kind === "strongRef" || available.kind === "strongRefVector" || available.kind === "strongRefSet") {
      await beginCreate(available.pid, available.type);
    } else {
      onerror(`${available.name} cannot be added from the inspector yet`);
    }
  }

  async function deleteObject() {
    try {
      await client.remove(object.id);
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      if (message.includes("weak reference") && confirm(`${message}\n\nDelete anyway? References to it will be left dangling.`)) {
        await attempt(() => client.remove(object.id, true));
      } else if (!message.includes("weak reference")) {
        onerror(message);
      }
    }
  }

  async function moveWithinParent(delta: number) {
    if (object.parent === null) return;
    await attempt(async () => {
      const siblings = await client.children(object.parent!, 0, 100000);
      const mine = siblings.items.find((s) => s.id === object.id);
      if (!mine) return;
      await client.move(object.parent!, object.parentPid, mine.index, mine.index + delta);
    });
  }

  const optionalMissing = $derived(object.available.filter((a) => a.optional));
  const requiredMissing = $derived(object.available.filter((a) => !a.optional));
  const isEssence = $derived(object.class === "EssenceData" || object.properties.some((p) => p.kind === "stream" && p.name === "Data"));
</script>

<section class="panel">
  <header>
    <div>
      <h2>{object.label}</h2>
      <div class="subtitle">
        {object.class} · object {object.id}
        {#if !object.attached}<span class="badge warn">detached</span>{/if}
      </div>
    </div>
    <div class="actions">
      {#if object.class.endsWith("Mob") && object.properties.some((p) => p.name === "Slots")}
        <button onclick={() => ontimeline(object.id)}>Timeline</button>
      {/if}
      {#if object.parent !== null && object.parent !== 0}
        <button onclick={() => onselect(object.parent!)} title="Select parent">Parent</button>
        <button onclick={() => moveWithinParent(-1)} title="Move up (vectors only)">↑</button>
        <button onclick={() => moveWithinParent(1)} title="Move down (vectors only)">↓</button>
        <button class="danger" onclick={deleteObject}>Delete</button>
      {/if}
    </div>
  </header>

  {#if requiredMissing.length > 0}
    <div class="notice">Missing required: {requiredMissing.map((r) => r.name).join(", ")}</div>
  {/if}

  <div class="properties">
    {#each object.properties as p (p.pid)}
      <div class="row">
        <div class="name" title={`PID ${p.pid.toString(16).padStart(4, "0")} · ${p.type ?? "unknown type"}`}>
          {p.name}
          {#if p.optional === false}<span class="required" title="Required">*</span>{/if}
        </div>
        <div class="value">
          {#if p.kind === "data" && p.value}
            {#if p.error}<div class="error">{p.error}</div>{/if}
            <ValueEditor value={p.value} typeId={p.type} types={object.types} readonly={!!p.error} onchange={(v) => setValue(p, v)} />
          {:else if p.kind === "strongRef" && p.children}
            {#each p.children as child (child.id)}
              <button class="link" onclick={() => onselect(child.id)}>{child.label} <span class="muted">({child.class})</span></button>
            {/each}
          {:else if p.kind === "strongRefVector" || p.kind === "strongRefSet"}
            <span class="muted">{p.count} {p.kind === "strongRefSet" ? "in set" : "items"}</span>
            <button class="small" onclick={() => beginCreate(p.pid, p.type)}>Add…</button>
          {:else if p.kind === "weakRef" && p.target}
            <span class="weak">
              {#if p.target.id !== null}
                <button class="link" onclick={() => onselect(p.target!.id!)}>{p.target.label}</button>
              {:else}
                <span class:error={p.target.resolved === "missing"} class="mono">{p.target.label}</span>
                <span class="muted">{p.target.resolved === "builtin" ? "(built-in definition)" : "(unresolved)"}</span>
              {/if}
              <select onfocus={() => loadCandidates(p)} onchange={(e) => setWeak(p, (e.currentTarget as HTMLSelectElement).value)} value="">
                <option value="" disabled>Change…</option>
                {#each candidates[p.pid] ?? [] as c (c.id)}
                  <option value={String(c.id)}>{c.label} ({c.class})</option>
                {/each}
              </select>
            </span>
          {:else if p.kind === "weakRefCollection" && p.targets}
            <div class="targets">
              {#each p.targets as t, i (i)}
                {#if t.id !== null}
                  <button class="link" onclick={() => onselect(t.id!)}>{t.label}</button>
                {:else}
                  <span class="mono muted">{t.label}</span>
                {/if}
              {/each}
            </div>
          {:else if p.kind === "stream"}
            <span class="muted">{p.size?.toLocaleString()} bytes</span>
            {#if isEssence}
              <button class="small" onclick={() => onessence(object.id, "extract")}>Extract…</button>
              <button class="small" onclick={() => onessence(object.id, "replace")}>Replace…</button>
            {/if}
          {:else}
            <span class="mono muted">{formatValue(p.value)}</span>
          {/if}
        </div>
        <div class="row-actions">
          {#if p.optional}
            <button class="icon" title="Remove property" onclick={() => removeProperty(p)}>✕</button>
          {/if}
        </div>
      </div>
    {/each}
  </div>

  {#if createFor}
    <div class="create">
      <span>Add which class?</span>
      {#each createFor.classes as cls (cls.id)}
        <button class="small" onclick={() => create(cls.name)}>{cls.name}</button>
      {/each}
      <button class="small" onclick={() => (createFor = null)}>Cancel</button>
    </div>
  {/if}

  {#if optionalMissing.length > 0 || requiredMissing.length > 0}
    <div class="add">
      <select bind:value={addPid}>
        <option value={null}>Add property…</option>
        {#each [...requiredMissing, ...optionalMissing] as a (a.pid)}
          <option value={a.pid}>{a.name}{a.optional ? "" : " (required)"}</option>
        {/each}
      </select>
      <button class="small" disabled={addPid === null} onclick={addProperty}>Add</button>
    </div>
  {/if}
</section>

<style>
  .panel {
    padding: 12px 16px;
    overflow: auto;
    height: 100%;
    box-sizing: border-box;
  }
  header {
    display: flex;
    justify-content: space-between;
    align-items: start;
    gap: 12px;
    margin-bottom: 12px;
  }
  h2 {
    margin: 0;
    font-size: 16px;
    word-break: break-all;
  }
  .subtitle {
    color: var(--muted);
    font-size: 12px;
    margin-top: 2px;
  }
  .actions {
    display: flex;
    gap: 4px;
    flex-shrink: 0;
  }
  .properties {
    display: grid;
    grid-template-columns: minmax(140px, 30%) 1fr 24px;
    gap: 6px 12px;
  }
  .row {
    display: contents;
  }
  .name {
    font-size: 13px;
    padding-top: 3px;
    color: var(--text);
    overflow-wrap: anywhere;
  }
  .required {
    color: var(--danger);
    margin-left: 2px;
  }
  .value {
    min-width: 0;
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    align-items: center;
  }
  .weak,
  .targets {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    align-items: center;
  }
  .notice {
    background: var(--warn-bg);
    color: var(--warn);
    padding: 6px 8px;
    border-radius: 4px;
    margin-bottom: 10px;
    font-size: 12px;
  }
  .create,
  .add {
    margin-top: 14px;
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
    align-items: center;
  }
  .badge.warn {
    color: var(--warn);
    margin-left: 6px;
  }
  .error {
    color: var(--danger);
    font-size: 12px;
  }
  .muted {
    color: var(--muted);
    font-size: 12px;
  }
  .mono {
    font-family: var(--mono);
    word-break: break-all;
  }
</style>
