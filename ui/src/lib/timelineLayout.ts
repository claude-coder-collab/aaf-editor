import type { EditRate, Timeline, TimelineItem, TimelineTrack } from "./rpc";

export interface Row {
  track: TimelineTrack;
  label: string;
  y: number;
  height: number;
  /// Multiply a position in this track's edit units by `scale` to get base-rate units.
  scale: number;
  /// `maxEnd[i]` is the largest item end among items 0…i, in track units; it never decreases.
  maxEnd: Float64Array;
  /// True when item starts never decrease, so the items after a range can be skipped.
  sorted: boolean;
}

export interface Layout {
  rows: Row[];
  baseRate: EditRate;
  /// Duration in base-rate units.
  duration: number;
  height: number;
}

export const RULER_HEIGHT = 26;

const HEIGHTS: Record<string, number> = { picture: 38, sound: 38 };
const ORDER: Record<string, number> = { picture: 0, sound: 1, event: 2 };
const PREFIX: Record<string, string> = { picture: "V", sound: "A", timecode: "TC", edgecode: "EC", descriptiveMetadata: "DM", data: "D" };

export function rateValue(rate: EditRate): number {
  return rate.den > 0 ? rate.num / rate.den : 0;
}

export function layout(timeline: Timeline): Layout {
  const base = timeline.tracks.find((t) => t.kind === "picture" && t.slotKind === "timeline") ?? timeline.tracks.find((t) => t.slotKind === "timeline") ?? timeline.tracks[0];
  const baseRate = base?.editRate ?? { num: 25, den: 1 };
  const counters = new Map<string, number>();
  let y = RULER_HEIGHT;
  let duration = 0;
  const rows: Row[] = [];
  const category = (t: TimelineTrack) => ORDER[t.slotKind === "event" ? "event" : t.kind] ?? 3;
  const ordered = timeline.tracks.map((track, index) => ({ track, index })).sort((a, b) => category(a.track) - category(b.track) || a.index - b.index);
  for (const { track } of ordered) {
    const key = track.slotKind === "event" ? "event" : track.kind;
    const n = (counters.get(key) ?? 0) + 1;
    counters.set(key, n);
    const prefix = track.slotKind === "event" ? "M" : (PREFIX[track.kind] ?? "?");
    const height = track.slotKind === "event" ? 26 : (HEIGHTS[track.kind] ?? 18);
    const scale = rateValue(track.editRate) > 0 && rateValue(baseRate) > 0 ? rateValue(baseRate) / rateValue(track.editRate) : 1;
    const maxEnd = new Float64Array(track.items.length);
    let sorted = true;
    let end = -Infinity;
    track.items.forEach((item, i) => {
      end = Math.max(end, item.start + Math.max(item.length, 0));
      maxEnd[i] = end;
      if (i > 0 && item.start < track.items[i - 1]!.start) sorted = false;
    });
    rows.push({ track, label: `${prefix}${n}`, y, height, scale, maxEnd, sorted });
    duration = Math.max(duration, track.length * scale, end * scale);
    y += height + 1;
  }
  return { rows, baseRate, duration, height: y };
}

const STEPS_SECONDS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];

/// A ruler step in base-rate units that leaves at least `minPixels` between ticks.
export function tickStep(pixelsPerUnit: number, fps: number, minPixels = 90): number {
  if (pixelsPerUnit <= 0) {
    return 1;
  }
  for (const frames of [1, 2, 5, 10]) {
    if (frames < fps && frames * pixelsPerUnit >= minPixels) return frames;
  }
  for (const seconds of STEPS_SECONDS) {
    const units = Math.max(1, Math.round(seconds * fps));
    if (units * pixelsPerUnit >= minPixels) return units;
  }
  return Math.max(1, Math.round(7200 * fps));
}

/// The index range [first, last) of a row's items that may overlap base-rate positions `from`…`to`.
export function visibleRange(row: Row, from: number, to: number): [number, number] {
  const items = row.track.items;
  const lo = from / row.scale;
  const hi = to / row.scale;
  let first = 0;
  let last = items.length;
  while (first < last) {
    const mid = (first + last) >> 1;
    if (row.maxEnd[mid]! < lo) first = mid + 1;
    else last = mid;
  }
  if (!row.sorted) return [first, items.length];
  let end = items.length;
  let a = first;
  while (a < end) {
    const mid = (a + end) >> 1;
    if (items[mid]!.start <= hi) a = mid + 1;
    else end = mid;
  }
  return [first, end];
}

/// Merges runs of narrow rectangles of the same colour into single rectangles before they are drawn.
export class RunMerger {
  private run: { left: number; right: number; color: string } | null = null;

  constructor(private readonly fill: (left: number, width: number, color: string) => void) {}

  add(left: number, width: number, color: string): void {
    const right = left + width;
    if (this.run && this.run.color === color && left <= this.run.right + 0.5) {
      this.run.right = Math.max(this.run.right, right);
      return;
    }
    this.flush();
    this.run = { left, right, color };
  }

  flush(): void {
    if (this.run) this.fill(this.run.left, Math.max(1, this.run.right - this.run.left), this.run.color);
    this.run = null;
  }
}

export interface Hit {
  row: Row;
  item: TimelineItem;
}

/// Finds the item under a point; transitions and markers win over the clips they overlap.
export function hitTest(l: Layout, x: number, y: number, pixelsPerUnit: number, viewStart: number, headerWidth: number): Hit | null {
  const row = l.rows.find((r) => y >= r.y && y < r.y + r.height);
  if (!row || x < headerWidth) {
    return null;
  }
  const position = (x - headerWidth) / pixelsPerUnit + viewStart;
  const reach = 6 / pixelsPerUnit;
  const [first, last] = visibleRange(row, position - reach, position + reach);
  let found: TimelineItem | null = null;
  for (let i = first; i < last; i++) {
    const item = row.track.items[i]!;
    const start = item.start * row.scale;
    const end = (item.start + Math.max(item.length, 0)) * row.scale;
    const isPoint = item.kind === "marker" || item.kind === "event";
    const slack = isPoint ? 6 / pixelsPerUnit : 0;
    if (position >= start - slack && (position < end || (isPoint && position <= start + slack))) {
      if (!found || item.kind === "transition" || isPoint) found = item;
    }
  }
  return found ? { row, item: found } : null;
}
