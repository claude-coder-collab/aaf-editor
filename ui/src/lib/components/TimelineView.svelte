<script lang="ts">
  import type { RpcClient, SourceChain, Timeline, TimelineItem } from "../rpc";
  import { formatTimecode, nominalFps } from "../timecode";
  import { hitTest, layout, rateValue, RULER_HEIGHT, tickStep, type Layout } from "../timelineLayout";

  interface Props {
    client: RpcClient;
    mob: number;
    version: number;
    selected: number | null;
    onselect: (id: number) => void;
  }

  let { client, mob, version, selected, onselect }: Props = $props();

  const HEADER = 150;
  let timeline = $state<Timeline | null>(null);
  let error = $state("");
  let canvas: HTMLCanvasElement | undefined = $state();
  let width = $state(800);
  let height = $state(300);
  let pixelsPerUnit = $state(2);
  let viewStart = $state(0);
  let scrollTop = $state(0);
  let hover = $state<{ x: number; y: number; item: TimelineItem; row: string } | null>(null);
  let chain = $state<SourceChain | null>(null);
  let fitted = false;

  const view = $derived<Layout | null>(timeline ? layout(timeline) : null);
  const fps = $derived(timeline?.timecode?.fps || (view ? nominalFps(view.baseRate.num, view.baseRate.den) : 25) || 25);
  const drop = $derived(timeline?.timecode?.drop ?? false);
  const tcStart = $derived(timeline?.timecode?.start ?? 0);
  const contentWidth = $derived(view ? view.duration * pixelsPerUnit : 0);

  $effect(() => {
    void version;
    const id = mob;
    client
      .timeline(id)
      .then((t) => {
        timeline = t;
        error = "";
      })
      .catch((e: unknown) => (error = e instanceof Error ? e.message : String(e)));
  });

  $effect(() => {
    void mob;
    fitted = false;
    viewStart = 0;
  });

  $effect(() => {
    if (view && !fitted && width > HEADER) {
      fit();
      fitted = true;
    }
  });

  function fit() {
    if (!view || view.duration <= 0) return;
    pixelsPerUnit = Math.max(0.0005, (width - HEADER - 20) / view.duration);
    viewStart = 0;
  }

  function zoom(factor: number, anchorX = HEADER + (width - HEADER) / 2) {
    const anchor = (anchorX - HEADER) / pixelsPerUnit + viewStart;
    pixelsPerUnit = Math.min(200, Math.max(0.0005, pixelsPerUnit * factor));
    viewStart = Math.max(0, anchor - (anchorX - HEADER) / pixelsPerUnit);
  }

  function color(name: string): string {
    return canvas ? getComputedStyle(canvas).getPropertyValue(name).trim() || "#888" : "#888";
  }

  function itemColor(item: TimelineItem, kind: string): string {
    if (item.kind === "filler") return "transparent";
    if (item.source && !item.source.original && item.source.mob === null) return color("--danger");
    if (item.kind === "operationGroup") return color("--tl-effect");
    if (item.kind === "timecode" || item.kind === "pulldown" || item.kind === "edgecode") return color("--tl-code");
    if (item.source?.mobKind === "composition" || item.kind === "nestedScope" || item.kind === "sequence") return color("--tl-nested");
    return kind === "sound" ? color("--tl-audio") : color("--tl-video");
  }

  function draw() {
    if (!canvas || !view) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.floor(width * dpr);
    canvas.height = Math.floor(height * dpr);
    const g = canvas.getContext("2d");
    if (!g) return;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, width, height);
    g.font = "11px system-ui, sans-serif";
    g.textBaseline = "middle";
    const text = color("--text");
    const muted = color("--muted");
    const border = color("--border");
    const accent = color("--accent");
    const x = (units: number) => HEADER + (units - viewStart) * pixelsPerUnit;

    g.save();
    g.translate(0, -scrollTop);
    for (const row of view.rows) {
      g.fillStyle = color("--panel");
      g.fillRect(HEADER, row.y, width - HEADER, row.height);
      for (const item of row.track.items) {
        const left = x(item.start * row.scale);
        const w = Math.max(item.length * row.scale * pixelsPerUnit, 1);
        if (left > width || left + w < HEADER) continue;
        const top = row.y + 2;
        const h = row.height - 4;
        if (item.kind === "marker" || item.kind === "event") {
          g.fillStyle = color("--warn");
          g.beginPath();
          g.moveTo(left, top);
          g.lineTo(left + 6, top + h / 2);
          g.lineTo(left, top + h);
          g.lineTo(left - 6, top + h / 2);
          g.closePath();
          g.fill();
          continue;
        }
        if (item.kind === "transition") continue;
        const fill = itemColor(item, row.track.kind);
        if (fill !== "transparent") {
          g.fillStyle = fill;
          g.fillRect(left, top, w, h);
        } else {
          g.strokeStyle = border;
          g.setLineDash([3, 3]);
          g.strokeRect(left + 0.5, top + 0.5, w - 1, h - 1);
          g.setLineDash([]);
        }
        if (item.object === selected) {
          g.strokeStyle = accent;
          g.lineWidth = 2;
          g.strokeRect(left + 1, top + 1, w - 2, h - 2);
          g.lineWidth = 1;
        }
        if (w > 30 && item.kind !== "filler") {
          g.save();
          g.beginPath();
          g.rect(left + 4, top, w - 8, h);
          g.clip();
          g.fillStyle = "#fff";
          g.fillText(item.label, left + 5, top + h / 2);
          g.restore();
        }
      }
      for (const item of row.track.items) {
        if (item.kind !== "transition") continue;
        const left = x(item.start * row.scale);
        const w = Math.max(item.length * row.scale * pixelsPerUnit, 3);
        const top = row.y + 2;
        const h = row.height - 4;
        g.fillStyle = color("--tl-transition");
        g.globalAlpha = 0.85;
        g.fillRect(left, top, w, h);
        g.globalAlpha = 1;
        g.strokeStyle = "#fff";
        g.beginPath();
        g.moveTo(left, top);
        g.lineTo(left + w, top + h);
        g.moveTo(left, top + h);
        g.lineTo(left + w, top);
        g.stroke();
        if (item.object === selected) {
          g.strokeStyle = accent;
          g.lineWidth = 2;
          g.strokeRect(left + 1, top + 1, w - 2, h - 2);
          g.lineWidth = 1;
        }
      }
      g.fillStyle = color("--bg");
      g.fillRect(0, row.y, HEADER, row.height);
      g.fillStyle = text;
      g.fillText(`${row.label}  ${row.track.name || row.track.kind}`, 8, row.y + row.height / 2 - (row.height > 30 ? 6 : 0));
      if (row.height > 30) {
        g.fillStyle = muted;
        const effects = row.track.effects.map((e) => e.name).filter(Boolean).join(", ");
        g.fillText(effects || `${row.track.editRate.num}/${row.track.editRate.den}`, 8, row.y + row.height / 2 + 8);
      }
    }
    g.restore();

    g.fillStyle = color("--bg");
    g.fillRect(0, 0, width, RULER_HEIGHT);
    g.strokeStyle = border;
    g.beginPath();
    g.moveTo(0, RULER_HEIGHT - 0.5);
    g.lineTo(width, RULER_HEIGHT - 0.5);
    g.stroke();
    const step = tickStep(pixelsPerUnit, fps);
    const baseFps = rateValue(view.baseRate) || fps;
    const first = Math.floor(viewStart / step) * step;
    g.fillStyle = muted;
    for (let t = first; x(t) < width; t += step) {
      const px = x(t);
      if (px < HEADER) continue;
      g.strokeStyle = border;
      g.beginPath();
      g.moveTo(px + 0.5, RULER_HEIGHT - 8);
      g.lineTo(px + 0.5, RULER_HEIGHT);
      g.stroke();
      const frames = Math.round((t * fps) / baseFps) + tcStart;
      g.fillText(formatTimecode(frames, fps, drop), px + 3, RULER_HEIGHT / 2 - 2);
    }
    g.fillStyle = muted;
    g.fillText(`${view.baseRate.num}/${view.baseRate.den} fps`, 8, RULER_HEIGHT / 2);
  }

  $effect(() => {
    void [view, width, height, pixelsPerUnit, viewStart, scrollTop, selected];
    draw();
  });

  function wheel(event: WheelEvent) {
    event.preventDefault();
    if (event.ctrlKey || event.metaKey) {
      zoom(event.deltaY < 0 ? 1.25 : 0.8, event.offsetX);
    } else if (event.shiftKey || Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      viewStart = Math.max(0, viewStart + (event.deltaX || event.deltaY) / pixelsPerUnit);
    } else if (view) {
      scrollTop = Math.max(0, Math.min(view.height - height + 10, scrollTop + event.deltaY));
    }
  }

  function pick(event: MouseEvent) {
    if (!view) return null;
    return hitTest(view, event.offsetX, event.offsetY + scrollTop, pixelsPerUnit, viewStart, HEADER);
  }

  async function click(event: MouseEvent) {
    const hit = pick(event);
    if (!hit) return;
    onselect(hit.item.object);
    chain = hit.item.kind === "sourceClip" ? await client.resolve(hit.item.object).catch(() => null) : null;
  }

  function move(event: MouseEvent) {
    const hit = pick(event);
    hover = hit ? { x: event.offsetX, y: event.offsetY, item: hit.item, row: hit.row.label } : null;
  }

  function describe(item: TimelineItem, rowLabel: string): string {
    const lines = [`${rowLabel} · ${item.class}: ${item.label}`, `start ${item.start}, length ${item.hasLength ? item.length : "—"}`];
    if (item.source) {
      lines.push(item.source.original ? "original source" : `${item.source.mob === null ? "MISSING " : ""}${item.source.mobKind} mob, slot ${item.source.slotId}, from ${item.source.startTime}`);
    }
    if (item.comment) lines.push(item.comment);
    return lines.join("\n");
  }
</script>

<div class="timeline">
  <div class="controls">
    <strong>{timeline?.name || "Timeline"}</strong>
    <span class="muted">{timeline ? `${timeline.kind} · ${timeline.tracks.length} tracks` : ""}</span>
    <button class="small" onclick={() => zoom(1.5)} title="Zoom in (Ctrl+wheel)">+</button>
    <button class="small" onclick={() => zoom(1 / 1.5)} title="Zoom out">−</button>
    <button class="small" onclick={fit}>Fit</button>
    {#if chain}
      <span class="chain" title={chain.essence?.locators.join("\n") ?? ""}>
        Source: {chain.links.map((l) => `${l.name || l.kind} (${l.kind})`).join(" → ")}
        {#if chain.status !== "resolved"}<span class="bad">{chain.status}</span>{/if}
        {#if chain.essence}· {chain.essence.embedded ? "embedded" : chain.essence.locators[0] ?? "no locator"}{/if}
      </span>
    {/if}
  </div>
  {#if error}
    <p class="error">{error}</p>
  {/if}
  {#if timeline?.warnings.length}
    <p class="warn">{timeline.warnings.join("; ")}</p>
  {/if}
  <div class="surface" bind:clientWidth={width} bind:clientHeight={height}>
    <canvas bind:this={canvas} style:width="{width}px" style:height="{height}px" onwheel={wheel} onclick={click} onmousemove={move} onmouseleave={() => (hover = null)}></canvas>
    {#if view && contentWidth > width - HEADER}
      <input
        class="scroll"
        type="range"
        min="0"
        max={Math.max(0, view.duration - (width - HEADER) / pixelsPerUnit)}
        step="any"
        bind:value={viewStart}
        aria-label="Scroll timeline"
      />
    {/if}
    {#if hover}
      <div class="tooltip" style:left="{Math.min(hover.x + 12, width - 260)}px" style:top="{hover.y + 14}px">{describe(hover.item, hover.row)}</div>
    {/if}
  </div>
</div>

<style>
  .timeline {
    display: flex;
    flex-direction: column;
    height: 100%;
    min-height: 0;
    --tl-video: #3f6fb5;
    --tl-audio: #3c8a64;
    --tl-effect: #7a52b3;
    --tl-transition: #d98b2b;
    --tl-code: #6b7280;
    --tl-nested: #2f8f9d;
  }
  .controls {
    display: flex;
    gap: 8px;
    align-items: center;
    padding: 4px 8px;
    border-bottom: 1px solid var(--border);
    font-size: 12px;
    flex-wrap: wrap;
  }
  .chain {
    color: var(--muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 60%;
  }
  .bad,
  .error {
    color: var(--danger);
  }
  .warn {
    color: var(--warn);
    font-size: 12px;
    margin: 2px 8px;
  }
  .surface {
    position: relative;
    flex: 1;
    min-height: 0;
    overflow: hidden;
  }
  canvas {
    display: block;
  }
  .scroll {
    position: absolute;
    left: 150px;
    right: 8px;
    bottom: 2px;
    width: calc(100% - 158px);
  }
  .tooltip {
    position: absolute;
    white-space: pre;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 11px;
    pointer-events: none;
    box-shadow: 0 2px 8px rgb(0 0 0 / 0.15);
    z-index: 2;
  }
  .muted {
    color: var(--muted);
  }
</style>
