<script lang="ts">
  import { untrack } from "svelte";

  import type { ChangeSet, MobSummary, RpcClient, SourceChain, Timeline, TimelineItem } from "../rpc";
  import { dragZone, snap } from "../snap";
  import { formatTimecode, nominalFps } from "../timecode";
  import { badgeText, channelFormat, hitTest, renderedLabel, holds, layout, rateValue, RULER_HEIGHT, RunMerger, selectionTarget, tickStep, visibleRange, type Layout, type Row } from "../timelineLayout";
  import { addPending, mergeTimeline, type Pending } from "../timelineMerge";

  interface Props {
    client: RpcClient;
    mob: number;
    selected: number | null;
    mobs: MobSummary[];
    onselect: (id: number) => void;
    onerror: (message: string) => void;
    onwarning: (message: string) => void;
  }

  let { client, mob, selected, mobs, onselect, onerror, onwarning }: Props = $props();

  const HEADER = 150;
  const SNAP_PIXELS = 8;
  /// Distance of an effects badge from its clip's left edge; clicks nearer the edge trim instead.
  const BADGE_INSET = 8;
  /// Where each effects badge was last drawn (canvas x), by item, so clicks on a badge select the effect.
  const badges = new Map<number, { x0: number; x1: number }>();
  let timeline = $state.raw<Timeline | null>(null);
  let error = $state("");
  let canvas: HTMLCanvasElement | undefined = $state();
  let width = $state(800);
  let height = $state(300);
  let pixelsPerUnit = $state(2);
  let viewStart = $state(0);
  let scrollTop = $state(0);
  let playhead = $state(0);
  let selectedSlot = $state<number | null>(null);
  let hover = $state.raw<{ x: number; y: number; item: TimelineItem; row: string } | null>(null);
  let chain = $state<SourceChain | null>(null);
  let drag = $state.raw<{ zone: "head" | "tail" | "body"; item: TimelineItem; row: Row; startX: number; x: number; y: number; ripple: boolean } | null>(null);
  let sourceMob = $state<number | null>(null);
  let active = $state(false);
  let container: HTMLDivElement | undefined = $state();
  let fitted = false;

  const view = $derived<Layout | null>(timeline ? layout(timeline) : null);
  const fps = $derived(timeline?.timecode?.fps || (view ? nominalFps(view.baseRate.num, view.baseRate.den) : 25) || 25);
  const drop = $derived(timeline?.timecode?.drop ?? false);
  const tcStart = $derived(timeline?.timecode?.start ?? 0);
  const contentWidth = $derived(view ? view.duration * pixelsPerUnit : 0);
  const selectedRow = $derived(view?.rows.find((r) => r.track.slot === selectedSlot) ?? null);
  const selectedItemRow = $derived(view?.rows.find((r) => r.track.items.some((i) => holds(i, selected))) ?? null);
  const selectedItem = $derived(selectedItemRow?.track.items.find((i) => holds(i, selected)) ?? null);
  const editRow = $derived(selectedItemRow ?? selectedRow);
  const sources = $derived(mobs.filter((m) => m.kind === "master" || m.kind === "source"));

  let pending: Pending = null;
  let fetching = false;

  function invalidate(objects: readonly number[] | null) {
    pending = addPending(pending, objects);
    void fetchPending();
  }

  async function fetchPending() {
    if (fetching) return;
    fetching = true;
    try {
      while (pending) {
        const work = pending;
        pending = null;
        const id = mob;
        const base = timeline?.mob === id ? timeline : null;
        try {
          const next = await client.timeline(id, work.full || !base ? undefined : [...work.objects]);
          if (id !== mob) continue;
          const merged = mergeTimeline(timeline?.mob === id ? timeline : null, next);
          if (merged) {
            timeline = merged;
            error = "";
          } else {
            pending = addPending(pending, null);
          }
        } catch (e) {
          error = e instanceof Error ? e.message : String(e);
        }
      }
    } finally {
      fetching = false;
    }
  }

  $effect(() => {
    void mob;
    untrack(() => invalidate(null));
  });

  $effect(() =>
    client.onEvent((method, params) => {
      if (method === "doc.changed") invalidate((params as { changes: ChangeSet }).changes.objects);
      else if (method === "doc.opened") invalidate(null);
    }),
  );

  $effect(() => {
    void mob;
    fitted = false;
    viewStart = 0;
    playhead = 0;
    selectedSlot = null;
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

  const toX = (units: number) => HEADER + (units - viewStart) * pixelsPerUnit;
  const toUnits = (x: number) => (x - HEADER) / pixelsPerUnit + viewStart;
  const trackUnits = (row: Row, base: number) => Math.round(base / row.scale);

  const PALETTE = ["--text", "--muted", "--border", "--accent", "--danger", "--warn", "--bg", "--panel", "--selection", "--tl-effect", "--tl-code", "--tl-nested", "--tl-audio", "--tl-video", "--tl-transition"] as const;
  type Palette = Record<(typeof PALETTE)[number], string>;

  function readPalette(): Palette {
    const style = canvas ? getComputedStyle(canvas) : null;
    return Object.fromEntries(PALETTE.map((name) => [name, style?.getPropertyValue(name).trim() || "#888"])) as Palette;
  }

  function itemColor(item: TimelineItem, kind: string, c: Palette): string {
    if (item.kind === "filler") return "transparent";
    if (item.source && !item.source.original && item.source.mob === null) return c["--danger"];
    if (item.kind === "operationGroup" && item.clip === undefined) return c["--tl-effect"];
    if (item.kind === "timecode" || item.kind === "pulldown" || item.kind === "edgecode") return c["--tl-code"];
    if (item.source?.mobKind === "composition" || item.kind === "nestedScope" || item.kind === "sequence") return c["--tl-nested"];
    return kind === "sound" ? c["--tl-audio"] : c["--tl-video"];
  }

  /// Rendered audio over its clip: a ramp for fades, a shaded overlap for crossfades, hatching for seams and regions.
  function drawRendered(g: CanvasRenderingContext2D, kind: NonNullable<TimelineItem["rendered"]>, left: number, top: number, w: number, h: number) {
    g.save();
    g.beginPath();
    g.rect(left, top, w, h);
    g.clip();
    g.fillStyle = "rgb(0 0 0 / 0.35)";
    g.beginPath();
    if (kind === "fadeIn") {
      g.moveTo(left, top);
      g.lineTo(left + w, top);
      g.lineTo(left, top + h);
    } else if (kind === "fadeOut") {
      g.moveTo(left, top);
      g.lineTo(left + w, top);
      g.lineTo(left + w, top + h);
    } else if (kind === "crossfade") {
      g.moveTo(left, top);
      g.lineTo(left + w, top);
      g.lineTo(left + w / 2, top + h / 2);
      g.moveTo(left, top + h);
      g.lineTo(left + w, top + h);
      g.lineTo(left + w / 2, top + h / 2);
    } else {
      for (let x = left - h; x < left + w; x += 6) {
        g.moveTo(x, top + h);
        g.lineTo(x + h, top);
        g.lineTo(x + h + 2, top);
        g.lineTo(x + 2, top + h);
      }
    }
    g.fill();
    g.restore();
  }

  function editPoints(): number[] {
    if (!view) return [];
    const points = [playhead];
    for (const row of view.rows) {
      for (const item of row.track.items) {
        points.push(item.start * row.scale, (item.start + item.length) * row.scale);
      }
    }
    return points;
  }

  /// Base-rate position a drag would produce, with snapping.
  function dragTarget(d: NonNullable<typeof drag>): number {
    const tolerance = SNAP_PIXELS / pixelsPerUnit;
    const delta = (d.x - d.startX) / pixelsPerUnit;
    const points = editPoints();
    if (d.zone === "body") {
      return Math.max(0, snap(d.item.start * d.row.scale + delta, points, tolerance));
    }
    const edge = d.zone === "head" ? d.item.start * d.row.scale : (d.item.start + d.item.length) * d.row.scale;
    return snap(edge + delta, points, tolerance);
  }

  function rowAt(y: number): Row | null {
    return view?.rows.find((r) => y + scrollTop >= r.y && y + scrollTop < r.y + r.height) ?? null;
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
    const c = readPalette();
    const text = c["--text"];
    const muted = c["--muted"];
    const border = c["--border"];
    const accent = c["--accent"];
    const from = toUnits(HEADER) - 6 / pixelsPerUnit;
    const to = toUnits(width) + 6 / pixelsPerUnit;
    badges.clear();

    g.save();
    g.translate(0, -scrollTop);
    for (const row of view.rows) {
      if (row.y + row.height < scrollTop + RULER_HEIGHT || row.y > scrollTop + height) continue;
      g.fillStyle = row.track.slot === selectedSlot ? c["--selection"] : c["--panel"];
      g.fillRect(HEADER, row.y, width - HEADER, row.height);
      const top = row.y + 2;
      const h = row.height - 4;
      const runs = new RunMerger((left, w, fill) => {
        g.fillStyle = fill;
        g.fillRect(left, top, w, h);
      });
      const [first, last] = visibleRange(row, from, to);
      for (let i = first; i < last; i++) {
        const item = row.track.items[i]!;
        const left = toX(item.start * row.scale);
        const exact = item.length * row.scale * pixelsPerUnit;
        if (item.kind === "marker" || item.kind === "event") {
          runs.flush();
          g.fillStyle = item.object === selected ? accent : c["--warn"];
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
        const fill = itemColor(item, row.track.kind, c);
        const isSelected = holds(item, selected);
        const highlighted = isSelected || drag?.item.object === item.object;
        if (exact < 2 && !highlighted) {
          if (fill !== "transparent") runs.add(left, exact, fill);
          continue;
        }
        runs.flush();
        const w = Math.max(exact, 1);
        if (fill !== "transparent") {
          g.fillStyle = fill;
          g.globalAlpha = drag?.item.object === item.object ? 0.4 : 1;
          g.fillRect(left, top, w, h);
          g.globalAlpha = 1;
        } else {
          g.strokeStyle = border;
          g.setLineDash([3, 3]);
          g.strokeRect(left + 0.5, top + 0.5, w - 1, h - 1);
          g.setLineDash([]);
        }
        if (item.rendered) drawRendered(g, item.rendered, left, top, w, h);
        if (isSelected) {
          g.strokeStyle = accent;
          g.lineWidth = 2;
          g.strokeRect(left + 1, top + 1, w - 2, h - 2);
          g.lineWidth = 1;
        }
        const previous = i > 0 ? row.track.items[i - 1] : undefined;
        const covered = previous?.kind === "transition" ? Math.max(0, toX((previous.start + previous.length) * row.scale) - left) : 0;
        const visible = w - covered;
        let textLeft = left + covered + 5;
        const badge = visible >= 40 ? badgeText(item, visible - 60, (s) => g.measureText(s).width) : null;
        if (badge) {
          const x0 = left + covered + BADGE_INSET;
          const bw = g.measureText(badge).width + 8;
          const effectSelected = item.effects?.some((e) => e.object === selected) ?? false;
          g.fillStyle = effectSelected ? accent : "rgb(0 0 0 / 0.35)";
          g.beginPath();
          g.roundRect(x0, top + h / 2 - 7, bw, 14, 3);
          g.fill();
          g.fillStyle = "#fff";
          g.fillText(badge, x0 + 4, top + h / 2);
          badges.set(item.object, { x0, x1: x0 + bw });
          textLeft = x0 + bw + 5;
        }
        let textRight = left + w - 4;
        const format = channelFormat(item.channels);
        if (format && w >= 60) {
          const fw = g.measureText(format).width + 8;
          const fx = left + w - fw - 4;
          g.fillStyle = "rgb(0 0 0 / 0.35)";
          g.beginPath();
          g.roundRect(fx, top + h / 2 - 7, fw, 14, 3);
          g.fill();
          g.fillStyle = "#fff";
          g.fillText(format, fx + 4, top + h / 2);
          textRight = fx - 2;
        }
        if (w > 30 && item.kind !== "filler") {
          g.save();
          g.beginPath();
          g.rect(left + 4, top, textRight - left - 4, h);
          g.clip();
          g.fillStyle = "#fff";
          g.fillText(renderedLabel(item.rendered) || item.label, textLeft, top + h / 2);
          g.restore();
        }
      }
      runs.flush();
      for (let i = first; i < last; i++) {
        const item = row.track.items[i]!;
        if (item.kind !== "transition") continue;
        const left = toX(item.start * row.scale);
        const w = Math.max(item.length * row.scale * pixelsPerUnit, 3);
        g.fillStyle = c["--tl-transition"];
        g.globalAlpha = 0.85;
        g.fillRect(left, top, w, h);
        g.globalAlpha = 1;
        if (w >= 6) {
          g.strokeStyle = "#fff";
          g.beginPath();
          g.moveTo(left, top);
          g.lineTo(left + w, top + h);
          g.moveTo(left, top + h);
          g.lineTo(left + w, top);
          g.stroke();
        }
        if (item.object === selected) {
          g.strokeStyle = accent;
          g.lineWidth = 2;
          g.strokeRect(left + 1, top + 1, w - 2, h - 2);
          g.lineWidth = 1;
        }
      }
      g.fillStyle = row.track.slot === selectedSlot ? c["--selection"] : c["--bg"];
      g.fillRect(0, row.y, HEADER, row.height);
      g.fillStyle = text;
      g.fillText(`${row.label}  ${row.track.name || row.track.kind}`, 8, row.y + row.height / 2 - (row.height > 30 ? 6 : 0));
      if (row.height > 30) {
        g.fillStyle = muted;
        const effects = row.track.effects.map((e) => e.name).filter(Boolean).join(", ");
        const details = [channelFormat(row.track.channels), effects || `${row.track.editRate.num}/${row.track.editRate.den}`].filter(Boolean).join(" · ");
        g.fillText(details, 8, row.y + row.height / 2 + 8);
      }
    }
    if (drag) {
      const target = dragTarget(drag);
      const row = drag.zone === "body" ? (rowAt(drag.y) ?? drag.row) : drag.row;
      const start = drag.item.start * drag.row.scale;
      const end = (drag.item.start + drag.item.length) * drag.row.scale;
      const [from, to] = drag.zone === "body" ? [target, target + drag.item.length * drag.row.scale] : drag.zone === "head" ? [target, end] : [start, target];
      g.strokeStyle = accent;
      g.lineWidth = 2;
      g.setLineDash([4, 3]);
      g.strokeRect(toX(from), row.y + 2, Math.max(2, (to - from) * pixelsPerUnit), row.height - 4);
      g.setLineDash([]);
      g.lineWidth = 1;
    }
    g.restore();

    const px = toX(playhead);
    if (px >= HEADER && px <= width) {
      g.strokeStyle = c["--danger"];
      g.beginPath();
      g.moveTo(px + 0.5, 0);
      g.lineTo(px + 0.5, height);
      g.stroke();
    }

    g.fillStyle = c["--bg"];
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
    for (let t = first; toX(t) < width; t += step) {
      const x = toX(t);
      if (x < HEADER) continue;
      g.strokeStyle = border;
      g.beginPath();
      g.moveTo(x + 0.5, RULER_HEIGHT - 8);
      g.lineTo(x + 0.5, RULER_HEIGHT);
      g.stroke();
      g.fillText(formatTimecode(Math.round((t * fps) / baseFps) + tcStart, fps, drop), x + 3, RULER_HEIGHT / 2 - 2);
    }
    g.fillStyle = text;
    g.fillText(formatTimecode(Math.round((playhead * fps) / baseFps) + tcStart, fps, drop), 8, RULER_HEIGHT / 2);
  }

  $effect(() => {
    void [view, width, height, pixelsPerUnit, viewStart, scrollTop, selected, selectedSlot, playhead, drag];
    draw();
  });

  $effect(() => {
    if (!window.__aafTest) return;
    window.__aafTimeline = {
      tracks: () => view?.rows.map((r) => ({ slot: r.track.slot, label: r.label, kind: r.track.kind, channels: r.track.channels, items: r.track.items.map((i) => ({ object: i.object, kind: i.kind, start: i.start, length: i.length, clip: i.clip, effects: i.effects?.map((e) => e.name), channels: i.channels, rendered: i.rendered })) })) ?? [],
      itemRect: (object: number) => {
        const row = view?.rows.find((r) => r.track.items.some((i) => i.object === object));
        const item = row?.track.items.find((i) => i.object === object);
        if (!row || !item) return null;
        return { x: toX(item.start * row.scale), y: row.y - scrollTop, width: item.length * row.scale * pixelsPerUnit, height: row.height };
      },
      measure: (repeat: number) => {
        const start = performance.now();
        for (let n = 0; n < repeat; n++) draw();
        const drawn = performance.now();
        for (let n = 0; n < repeat; n++) hitTest(view!, width / 2, height / 2 + scrollTop, pixelsPerUnit, viewStart, HEADER);
        return { draw: (drawn - start) / repeat, pick: (performance.now() - drawn) / repeat };
      },
      zoom: (factor: number) => zoom(factor),
      show: (object: number, pixels: number) => {
        const row = view?.rows.find((r) => r.track.items.some((i) => i.object === object));
        const item = row?.track.items.find((i) => i.object === object);
        if (!row || !item || item.length <= 0) return;
        pixelsPerUnit = Math.min(200, pixels / (item.length * row.scale));
        viewStart = Math.max(0, item.start * row.scale - 40 / pixelsPerUnit);
        scrollTop = Math.max(0, row.y - RULER_HEIGHT - 4);
      },
    };
    return () => delete window.__aafTimeline;
  });

  async function op(params: Record<string, unknown>) {
    try {
      const result = await client.timelineOp(params);
      if (result.warnings?.length) onwarning(result.warnings.join(" "));
      return result;
    } catch (e) {
      onerror(e instanceof Error ? e.message : String(e));
      return null;
    }
  }

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

  function mousedown(event: MouseEvent) {
    if (event.button !== 0 || !view) return;
    if (event.offsetY < RULER_HEIGHT) {
      playhead = Math.max(0, Math.round(toUnits(event.offsetX)));
      return;
    }
    const row = rowAt(event.offsetY);
    if (event.offsetX < HEADER) {
      selectedSlot = row?.track.slot ?? null;
      return;
    }
    const hit = pick(event);
    if (!hit) return;
    selectedSlot = hit.row.track.slot;
    const badge = badges.get(hit.item.object);
    const effect = hit.item.effects?.[0];
    if (badge && effect && event.offsetX >= badge.x0 && event.offsetX <= badge.x1) {
      onselect(effect.object);
      return;
    }
    onselect(selectionTarget(hit.item));
    const editable = hit.row.track.slotKind === "timeline" && (hit.item.kind === "sourceClip" || hit.item.kind === "filler" || hit.item.kind === "operationGroup");
    if (!editable) return;
    const left = toX(hit.item.start * hit.row.scale);
    const zone = dragZone(event.offsetX, left, hit.item.length * hit.row.scale * pixelsPerUnit);
    if (zone === "body" && hit.item.kind === "filler") return;
    drag = { zone, item: hit.item, row: hit.row, startX: event.offsetX, x: event.offsetX, y: event.offsetY, ripple: event.altKey };
  }

  async function mouseup() {
    const d = drag;
    drag = null;
    if (!d || Math.abs(d.x - d.startX) < 3) {
      if (d && (d.item.kind === "sourceClip" || d.item.clip !== undefined)) chain = await client.resolve(selectionTarget(d.item)).catch(() => null);
      return;
    }
    const target = dragTarget(d);
    if (d.zone === "body") {
      const row = rowAt(d.y) ?? d.row;
      if (row.track.kind !== d.row.track.kind || row.track.slotKind !== "timeline") {
        onerror("Items can only move to a track of the same kind");
        return;
      }
      await op({ op: "move", item: d.item.object, toSlot: row.track.slot, position: trackUnits(row, target), ripple: d.ripple });
    } else {
      const edge = d.zone === "head" ? d.item.start : d.item.start + d.item.length;
      const delta = trackUnits(d.row, target) - edge;
      if (delta !== 0) await op({ op: "trim", item: d.item.object, edge: d.zone, delta, ripple: d.ripple });
    }
  }

  function mousemove(event: MouseEvent) {
    if (drag) {
      drag = { ...drag, x: event.offsetX, y: event.offsetY };
      hover = null;
      return;
    }
    const hit = pick(event);
    hover = hit ? { x: event.offsetX, y: event.offsetY, item: hit.item, row: hit.row.label } : null;
    if (canvas) {
      const left = hit ? toX(hit.item.start * hit.row.scale) : 0;
      const zone = hit ? dragZone(event.offsetX, left, hit.item.length * hit.row.scale * pixelsPerUnit) : "body";
      canvas.style.cursor = hit && hit.row.track.slotKind === "timeline" && zone !== "body" ? "ew-resize" : "default";
    }
  }

  const splitAtPlayhead = () => editRow && op({ op: "split", slot: editRow.track.slot, position: trackUnits(editRow, playhead) });
  const lift = () => selectedItem && op({ op: "lift", item: selectedItem.object });
  const rippleDelete = () => selectedItem && op({ op: "rippleDelete", item: selectedItem.object });

  async function addMarker() {
    const comment = prompt("Marker comment", "");
    if (comment !== null && view) await op({ op: "addMarker", mob, position: Math.round(playhead), comment });
  }

  async function addTrack(kind: "picture" | "sound") {
    const created = await op({ op: "addTrack", mob, kind, name: "" });
    if (created?.id !== undefined) selectedSlot = created.id;
  }

  async function removeTrack() {
    if (selectedRow && confirm(`Remove track ${selectedRow.label}?`)) {
      await op({ op: "removeTrack", slot: selectedRow.track.slot });
      selectedSlot = null;
    }
  }

  async function placeSource(insert: boolean) {
    if (!editRow || sourceMob === null) {
      onerror("Select a track and a source clip first");
      return;
    }
    const source = await client.timeline(sourceMob);
    const track = source.tracks.find((t) => t.kind === editRow.track.kind && t.slotKind === "timeline" && t.length > 0);
    if (!track) {
      onerror(`The source has no ${editRow.track.kind} track`);
      return;
    }
    const scale = rateValue(editRow.track.editRate) / rateValue(track.editRate);
    await op({
      op: insert ? "insertClip" : "overwriteClip",
      slot: editRow.track.slot,
      position: trackUnits(editRow, playhead),
      sourceMob,
      sourceSlot: track.slotId,
      sourceIn: 0,
      length: Math.max(1, Math.floor(track.length * scale)),
    });
  }

  async function relink() {
    const find = prompt("Relink media: text to find in locator URLs", "");
    if (!find) return;
    const replace = prompt(`Replace "${find}" with`, "");
    if (replace === null) return;
    const result = await op({ op: "relink", find, replace });
    if (result) onerror(`Relinked ${result.count ?? 0} locator(s)`);
  }

  function keydown(event: KeyboardEvent) {
    if (!active || event.target instanceof HTMLInputElement || event.target instanceof HTMLSelectElement || event.ctrlKey || event.metaKey) return;
    const step = event.shiftKey ? 10 : 1;
    switch (event.key) {
      case "s":
      case "S":
        void splitAtPlayhead();
        break;
      case "Delete":
      case "Backspace":
        void (event.shiftKey ? rippleDelete() : lift());
        break;
      case "m":
      case "M":
        void addMarker();
        break;
      case "ArrowLeft":
        playhead = Math.max(0, playhead - step);
        break;
      case "ArrowRight":
        playhead = playhead + step;
        break;
      default:
        return;
    }
    event.preventDefault();
  }

  function describe(item: TimelineItem, rowLabel: string): string {
    const lines = [`${rowLabel} · ${item.clip !== undefined ? "SourceClip" : item.class}: ${item.label}`, `start ${item.start}, length ${item.hasLength ? item.length : "—"}`];
    if (item.effects?.length) lines.push(`effects: ${item.effects.map((e) => e.name || "effect").join(", ")}`);
    if (item.channels) lines.push(`channels: ${channelFormat(item.channels)}`);
    if (item.rendered) lines.push(`rendered by Pro Tools: ${renderedLabel(item.rendered).toLowerCase()}${item.rendered === "region" ? " (cannot be edited)" : " (Lift replaces it with a cut)"}`);
    if (item.source) {
      lines.push(item.source.original ? "original source" : `${item.source.mob === null ? "MISSING " : ""}${item.source.mobKind} mob, slot ${item.source.slotId}, from ${item.source.startTime}`);
    }
    if (item.comment) lines.push(item.comment);
    return lines.join("\n");
  }
</script>

<svelte:window onkeydown={keydown} onmousedown={(e) => (active = !!container && container.contains(e.target as Node))} />

<div class="timeline" class:active bind:this={container}>
  <div class="controls">
    <strong>{timeline?.name || "Timeline"}</strong>
    <button class="small" onclick={() => zoom(1.5)} title="Zoom in (Ctrl+wheel)">+</button>
    <button class="small" onclick={() => zoom(1 / 1.5)} title="Zoom out">−</button>
    <button class="small" onclick={fit}>Fit</button>
    <span class="sep"></span>
    <button class="small" onclick={splitAtPlayhead} disabled={!editRow} title="Split at playhead (S)">Split</button>
    <button class="small" onclick={lift} disabled={!selectedItem} title="Lift (Delete)">Lift</button>
    <button class="small" onclick={rippleDelete} disabled={!selectedItem} title="Ripple delete (Shift+Delete)">Ripple</button>
    <button class="small" onclick={addMarker} title="Add marker at playhead (M)">Marker</button>
    <span class="sep"></span>
    <select class="small" bind:value={sourceMob} title="Source for insert/overwrite">
      <option value={null}>Source clip…</option>
      {#each sources as m (m.id)}
        <option value={m.id}>{m.name || "(unnamed)"} — {m.kind}</option>
      {/each}
    </select>
    <button class="small" onclick={() => placeSource(true)} disabled={sourceMob === null || !editRow} title="Insert at playhead on the selected track">Insert</button>
    <button class="small" onclick={() => placeSource(false)} disabled={sourceMob === null || !editRow} title="Overwrite at playhead on the selected track">Overwrite</button>
    <span class="sep"></span>
    <button class="small" onclick={() => addTrack("picture")}>+V</button>
    <button class="small" onclick={() => addTrack("sound")}>+A</button>
    <button class="small" onclick={removeTrack} disabled={!selectedRow}>−Track</button>
    <button class="small" onclick={relink}>Relink…</button>
    {#if chain}
      <span class="chain" title={chain.essence?.locators.join("\n") ?? ""}>
        {chain.links.map((l) => `${l.name || l.kind} (${l.kind})`).join(" → ")}
        {#if chain.status !== "resolved"}<span class="bad">{chain.status}</span>{/if}
        {#if chain.essence}· {chain.essence.embedded ? "embedded" : (chain.essence.locators[0] ?? "no locator")}{/if}
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
    <canvas
      bind:this={canvas}
      style:width="{width}px"
      style:height="{height}px"
      onwheel={wheel}
      onmousedown={mousedown}
      onmouseup={mouseup}
      onmousemove={mousemove}
      onmouseleave={() => ((hover = null), (drag = null))}
    ></canvas>
    {#if view && contentWidth > width - HEADER}
      <input class="scroll" type="range" min="0" max={Math.max(0, view.duration - (width - HEADER) / pixelsPerUnit)} step="any" bind:value={viewStart} aria-label="Scroll timeline" />
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
    outline: none;
    --tl-video: #3f6fb5;
    --tl-audio: #3c8a64;
    --tl-effect: #7a52b3;
    --tl-transition: #d98b2b;
    --tl-code: #6b7280;
    --tl-nested: #2f8f9d;
  }
  .timeline.active {
    box-shadow: inset 0 2px 0 var(--accent);
  }
  .controls {
    display: flex;
    gap: 6px;
    align-items: center;
    padding: 4px 8px;
    border-bottom: 1px solid var(--border);
    font-size: 12px;
    flex-wrap: wrap;
  }
  .controls select {
    max-width: 180px;
    font-size: 12px;
  }
  .sep {
    width: 1px;
    height: 16px;
    background: var(--border);
  }
  .chain {
    color: var(--muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 40%;
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
</style>
