<script lang="ts">
  export interface MenuItem {
    id: string;
    label: string;
    description?: string;
  }

  let { label, items, disabled = false, onpick }: { label: string; items: MenuItem[]; disabled?: boolean; onpick: (id: string) => void } = $props();

  let open = $state(false);
  let root: HTMLElement | undefined = $state();

  function pick(id: string) {
    open = false;
    onpick(id);
  }

  function outside(event: MouseEvent) {
    if (open && root && !root.contains(event.target as Node)) open = false;
  }

  function keydown(event: KeyboardEvent) {
    if (open && event.key === "Escape") {
      open = false;
      event.preventDefault();
    }
  }
</script>

<svelte:window onmousedown={outside} onkeydown={keydown} />

<div class="menu" bind:this={root}>
  <button aria-haspopup="menu" aria-expanded={open} {disabled} onclick={() => (open = !open)}>{label} ▾</button>
  {#if open && !disabled}
    <div class="popup" role="menu">
      {#each items as item (item.id)}
        <button role="menuitem" title={item.description} onclick={() => pick(item.id)}>{item.label}</button>
      {/each}
    </div>
  {/if}
</div>

<style>
  .menu {
    position: relative;
  }
  .popup {
    position: absolute;
    top: calc(100% + 2px);
    left: 0;
    z-index: 50;
    min-width: 220px;
    display: flex;
    flex-direction: column;
    padding: 4px 0;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    box-shadow: 0 4px 16px rgb(0 0 0 / 18%);
  }
  .popup button {
    border: none;
    border-radius: 0;
    background: none;
    text-align: left;
    padding: 4px 12px;
    white-space: nowrap;
  }
  .popup button:hover,
  .popup button:focus-visible {
    background: var(--selection);
  }
</style>
