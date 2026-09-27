<script lang="ts">
  import type { Diagnostic } from "../rpc";

  interface Props {
    diagnostics: Diagnostic[] | null;
    history: { items: string[]; position: number };
    onselect: (id: number) => void;
    onvalidate: () => void;
    ongoto: (position: number) => void;
  }

  let { diagnostics, history, onselect, onvalidate, ongoto }: Props = $props();
  let tab = $state<"diagnostics" | "history">("diagnostics");

  const counts = $derived({
    error: diagnostics?.filter((d) => d.severity === "error").length ?? 0,
    warning: diagnostics?.filter((d) => d.severity === "warning").length ?? 0,
    info: diagnostics?.filter((d) => d.severity === "info").length ?? 0,
  });
</script>

<section class="bottom">
  <nav>
    <button class:active={tab === "diagnostics"} onclick={() => (tab = "diagnostics")}>
      Diagnostics {#if diagnostics}<span class="muted">({counts.error} errors, {counts.warning} warnings, {counts.info} notes)</span>{/if}
    </button>
    <button class:active={tab === "history"} onclick={() => (tab = "history")}>History <span class="muted">({history.items.length})</span></button>
    {#if tab === "diagnostics"}<button class="small push" onclick={onvalidate}>Validate</button>{/if}
  </nav>
  <div class="content">
    {#if tab === "diagnostics"}
      {#if diagnostics === null}
        <p class="muted">Run Validate to check the file.</p>
      {:else if diagnostics.length === 0}
        <p class="ok">No problems found.</p>
      {:else}
        <ul>
          {#each diagnostics as d, i (i)}
            <li class={d.severity}>
              <span class="severity">{d.severity}</span>
              {#if d.object !== null}
                <button class="link" onclick={() => onselect(d.object!)}>object {d.object}</button>
              {/if}
              <span>{d.message}</span>
            </li>
          {/each}
        </ul>
      {/if}
    {:else}
      <ul>
        <li class:current={history.position === 0}>
          <button class="link" onclick={() => ongoto(0)}>Opened file</button>
        </li>
        {#each history.items as item, i (i)}
          <li class:current={history.position === i + 1} class:undone={i >= history.position}>
            <button class="link" onclick={() => ongoto(i + 1)}>{item}</button>
          </li>
        {/each}
      </ul>
    {/if}
  </div>
</section>

<style>
  .bottom {
    display: flex;
    flex-direction: column;
    height: 100%;
    border-top: 1px solid var(--border);
  }
  nav {
    display: flex;
    gap: 2px;
    border-bottom: 1px solid var(--border);
    padding: 0 6px;
    align-items: center;
  }
  nav button {
    border: none;
    background: none;
    padding: 6px 10px;
    color: var(--muted);
    cursor: pointer;
    border-bottom: 2px solid transparent;
  }
  nav button.active {
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  nav .push {
    margin-left: auto;
    border: 1px solid var(--border);
    color: var(--text);
    padding: 2px 10px;
  }
  .content {
    overflow: auto;
    flex: 1;
    padding: 4px 10px;
    font-size: 12px;
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  li {
    display: flex;
    gap: 8px;
    padding: 2px 0;
    align-items: baseline;
  }
  .severity {
    text-transform: uppercase;
    font-size: 10px;
    font-weight: 600;
    min-width: 56px;
  }
  .error .severity {
    color: var(--danger);
  }
  .warning .severity {
    color: var(--warn);
  }
  .info .severity {
    color: var(--muted);
  }
  .current {
    font-weight: 600;
  }
  .undone {
    opacity: 0.55;
  }
  .ok {
    color: var(--ok);
  }
  .muted {
    color: var(--muted);
  }
</style>
