import { describe, expect, it } from "vitest";

import type { Timeline, TimelineTrack } from "./rpc";
import { addPending, mergeTimeline } from "./timelineMerge";

function track(slot: number, label = "", warnings: string[] = []): TimelineTrack {
  return { slot, slotId: slot, name: label, physicalNumber: null, kind: "picture", slotKind: "timeline", editRate: { num: 25, den: 1 }, origin: 0, length: 0, segment: slot + 100, effects: [], items: [], warnings };
}

function timeline(tracks: TimelineTrack[], slots = tracks.map((t) => t.slot), partial = false, mob = 1): Timeline {
  return { mob, mobId: "m", name: "T", kind: "composition", tracks, slots, partial, warnings: tracks.flatMap((t) => t.warnings), timecode: null };
}

describe("mergeTimeline", () => {
  it("returns a full response as it is", () => {
    const next = timeline([track(1)]);
    expect(mergeTimeline(null, next)).toBe(next);
  });

  it("replaces changed tracks and keeps the others in slot order", () => {
    const current = timeline([track(1, "a"), track(2, "b"), track(3, "c")]);
    const merged = mergeTimeline(current, timeline([track(2, "B", ["w"]), track(4, "new")], [4, 1, 2, 3], true));
    expect(merged?.tracks.map((t) => t.name)).toEqual(["new", "a", "B", "c"]);
    expect(merged?.partial).toBe(false);
    expect(merged?.warnings).toEqual(["w"]);
  });

  it("drops tracks whose slots are gone", () => {
    const current = timeline([track(1), track(2)]);
    expect(mergeTimeline(current, timeline([], [2], true))?.tracks.map((t) => t.slot)).toEqual([2]);
  });

  it("needs a full fetch when a slot is unknown or the mob differs", () => {
    const current = timeline([track(1)]);
    expect(mergeTimeline(current, timeline([], [1, 5], true))).toBeNull();
    expect(mergeTimeline(current, timeline([], [1], true, 2))).toBeNull();
    expect(mergeTimeline(null, timeline([], [], true))).toBeNull();
  });
});

describe("addPending", () => {
  it("accumulates objects until a full refresh is needed", () => {
    let pending = addPending(null, [1, 2]);
    pending = addPending(pending, [2, 3]);
    expect(pending).toEqual({ full: false, objects: new Set([1, 2, 3]) });
    pending = addPending(pending, null);
    expect(pending).toEqual({ full: true });
    expect(addPending(pending, [4])).toEqual({ full: true });
  });
});
