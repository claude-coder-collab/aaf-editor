import type { Timeline } from "./rpc";

/// Applies a `timeline.get` response to the timeline currently shown. A partial response replaces only the tracks it
/// carries and keeps the rest; returns null when that is impossible and the whole timeline must be fetched again.
export function mergeTimeline(current: Timeline | null, next: Timeline): Timeline | null {
  if (!next.partial) return next;
  if (!current || current.mob !== next.mob) return null;
  const fresh = new Map(next.tracks.map((t) => [t.slot, t]));
  const kept = new Map(current.tracks.map((t) => [t.slot, t]));
  const tracks = [];
  for (const slot of next.slots) {
    const track = fresh.get(slot) ?? kept.get(slot);
    if (!track) return null;
    tracks.push(track);
  }
  return { ...next, tracks, partial: false, warnings: tracks.flatMap((t) => t.warnings) };
}

/// What must be refetched: nothing, everything, or the tracks holding some changed objects.
export type Pending = null | { full: true } | { full: false; objects: Set<number> };

export function addPending(pending: Pending, objects: readonly number[] | null): Pending {
  if (objects === null || pending?.full) return { full: true };
  const merged = new Set(pending?.objects ?? []);
  for (const o of objects) merged.add(o);
  return { full: false, objects: merged };
}
