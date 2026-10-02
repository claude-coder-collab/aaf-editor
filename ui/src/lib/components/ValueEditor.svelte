<script lang="ts">
  import type { LabelInfo, TaggedValue } from "../rpc";
  import { defaultValue, editorKind, formatValue, parseScalar, resolveType, withField, withItem, type Types } from "../values";
  import ValueEditor from "./ValueEditor.svelte";

  interface Props {
    value: TaggedValue;
    typeId: string | undefined;
    types: Types;
    readonly?: boolean;
    onchange: (value: TaggedValue) => void;
    labelFamily?: (auid: string) => Promise<LabelInfo[]>;
  }

  let { value, typeId, types, readonly = false, onchange, labelFamily }: Props = $props();

  const listId = `labels-${Math.random().toString(36).slice(2)}`;
  let family = $state<LabelInfo[] | null>(null);
  let choosing = $state(false);
  let choice = $state("");

  async function beginChoose() {
    if (!labelFamily || value.t !== "auid") return;
    family = await labelFamily(value.v);
    choice = "";
    choosing = true;
  }

  function choose() {
    const picked = family?.find((l) => l.name === choice.trim());
    if (!picked) return;
    choosing = false;
    if (value.t !== "auid" || picked.auid !== value.v) onchange({ t: "auid", v: picked.auid, name: picked.name });
  }

  const type = $derived(resolveType(types, typeId));
  const kind = $derived(editorKind(types, typeId));
  let draft = $state("");
  let error = $state("");
  let editing = $state(false);

  $effect(() => {
    if (!editing) {
      draft = value.t === "string" ? value.v : formatValue(value);
      error = "";
    }
  });

  function commitText() {
    editing = false;
    if (draft === (value.t === "string" ? value.v : formatValue(value))) {
      error = "";
      return;
    }
    const parsed = parseScalar(kind, draft, type);
    if (typeof parsed === "string") {
      error = parsed;
      editing = true;
      return;
    }
    error = "";
    onchange(parsed);
  }

  function keydown(event: KeyboardEvent) {
    if (event.key === "Enter") {
      (event.currentTarget as HTMLInputElement).blur();
    } else if (event.key === "Escape") {
      editing = false;
      draft = value.t === "string" ? value.v : formatValue(value);
      error = "";
      (event.currentTarget as HTMLInputElement).blur();
    }
  }

  function selectEnum(event: Event) {
    const selected = (event.currentTarget as HTMLSelectElement).value;
    const element = type?.elements?.find((e) => e.value === selected);
    if (!element) return;
    onchange(kind === "enum" ? { t: "enum", v: element.value, name: element.name } : { t: "extenum", v: element.value, name: element.name });
  }

  const currentEnum = $derived(value.t === "enum" || value.t === "extenum" ? value.v : "");
  const knownEnum = $derived(type?.elements?.some((e) => e.value === currentEnum) ?? false);
  const elementType = $derived(type?.element);
  const canResize = $derived(type?.kind !== "fixed_array");
</script>

{#if kind === "bool" && value.t === "bool"}
  <input type="checkbox" checked={value.v} disabled={readonly} onchange={(e) => onchange({ t: "bool", v: (e.currentTarget as HTMLInputElement).checked })} />
{:else if (kind === "enum" || kind === "extenum") && (value.t === "enum" || value.t === "extenum")}
  <select value={currentEnum} disabled={readonly} onchange={selectEnum}>
    {#if !knownEnum}
      <option value={currentEnum}>{formatValue(value)} (not a defined element)</option>
    {/if}
    {#each type?.elements ?? [] as element (element.value)}
      <option value={element.value}>{element.name}</option>
    {/each}
  </select>
{:else if kind === "record" && value.t === "record"}
  <div class="record">
    {#each value.fields as field (field.name)}
      {@const fieldType = type?.fields?.find((f) => f.name === field.name)?.type}
      <label class="field">
        <span class="field-name">{field.name}</span>
        <ValueEditor value={field.value} typeId={fieldType} {types} {readonly} {labelFamily} onchange={(v) => onchange(withField(value, field.name, v))} />
      </label>
    {/each}
  </div>
{:else if kind === "array" && value.t === "array"}
  <div class="array">
    {#each value.items as item, index (index)}
      <div class="array-item">
        <span class="index">{index}</span>
        <ValueEditor value={item} typeId={elementType} {types} {readonly} {labelFamily} onchange={(v) => onchange(withItem(value, index, v))} />
        {#if !readonly && canResize}
          <button class="icon" title="Remove element" onclick={() => onchange(withItem(value, index, null))}>−</button>
        {/if}
      </div>
    {:else}
      <span class="muted">empty</span>
    {/each}
    {#if !readonly && canResize}
      {@const fresh = defaultValue(types, elementType)}
      {#if fresh}
        <button class="small" onclick={() => onchange(withItem(value, value.items.length, fresh, true))}>Add element</button>
      {/if}
    {/if}
  </div>
{:else if kind === "int" || kind === "uint" || kind === "string" || kind === "auid" || kind === "mobid"}
  <span class="scalar">
    <input
      class:mono={kind !== "string"}
      class:id={kind === "auid"}
      class:umid={kind === "mobid"}
      class:invalid={error !== ""}
      type="text"
      spellcheck="false"
      bind:value={draft}
      readonly={readonly}
      onfocus={() => (editing = true)}
      onblur={commitText}
      onkeydown={keydown}
    />
    {#if (kind === "auid" || kind === "mobid") && !readonly}
      <button class="icon copy" title="Copy" onclick={() => navigator.clipboard?.writeText(draft)}>⧉</button>
    {/if}
    {#if value.t === "auid" && value.name}
      <span class="label-name" title="SMPTE label">{value.name}</span>
      {#if !readonly && labelFamily}
        {#if choosing}
          <input
            class="label-choice"
            type="text"
            list={listId}
            placeholder="Type to filter labels…"
            bind:value={choice}
            onchange={choose}
            onkeydown={(e) => e.key === "Escape" && (choosing = false)}
          />
          <datalist id={listId}>
            {#each family ?? [] as l (l.auid)}
              <option value={l.name}>{l.deprecated ? "deprecated" : ""}</option>
            {/each}
          </datalist>
          <button class="small" onclick={() => (choosing = false)}>Cancel</button>
        {:else}
          <button class="small" onclick={beginChoose}>Change…</button>
        {/if}
      {/if}
    {/if}
    {#if error}
      <span class="error">{error}</span>
    {/if}
  </span>
{:else}
  <span class="readonly mono" title={formatValue(value)}>{formatValue(value)}</span>
{/if}

<style>
  .record,
  .array {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding-left: 8px;
    border-left: 2px solid var(--border);
  }
  .field {
    display: grid;
    grid-template-columns: minmax(80px, max-content) 1fr;
    gap: 8px;
    align-items: start;
  }
  .field-name,
  .index {
    color: var(--muted);
    font-size: 12px;
    padding-top: 3px;
  }
  .array-item {
    display: flex;
    gap: 6px;
    align-items: start;
  }
  .scalar {
    display: flex;
    gap: 4px;
    align-items: center;
    flex-wrap: wrap;
    min-width: 0;
    flex: 1;
  }
  input[type="text"] {
    min-width: 12ch;
    width: 100%;
    max-width: 48ch;
  }
  input.umid {
    max-width: min(92ch, calc(100% - 28px));
  }
  input.id {
    max-width: min(40ch, calc(100% - 28px));
  }
  .mono {
    font-family: var(--mono);
  }
  .invalid {
    border-color: var(--danger);
  }
  .error {
    color: var(--danger);
    font-size: 12px;
    flex-basis: 100%;
  }
  .label-name {
    font-size: 12px;
    flex-basis: 100%;
  }
  .label-choice {
    max-width: 60ch;
  }
  .copy {
    flex-shrink: 0;
  }
  .readonly {
    word-break: break-all;
    color: var(--muted);
  }
  .muted {
    color: var(--muted);
    font-size: 12px;
  }
</style>
