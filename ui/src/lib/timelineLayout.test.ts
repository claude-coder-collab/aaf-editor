import { describe, expect, it } from "vitest";

import type { Timeline, TimelineItem, TimelineTrack } from "./rpc";
import { badgeText, channelFormat, hitTest, holds, layout, RULER_HEIGHT, RunMerger, selectionTarget, tickStep, visibleRange } from "./timelineLayout";

function item(object: number, kind: string, start: number, length: number): TimelineItem {
  return { object, kind, class: kind, start, length, hasLength: true, label: `item ${object}` };
}

function track(kind: string, rate: [number, number], items: TimelineItem[], slotKind = "timeline"): TimelineTrack {
  return { slot: 0, slotId: 1, name: "", physicalNumber: null, kind, slotKind, editRate: { num: rate[0], den: rate[1] }, origin: 0, length: Math.max(0, ...items.map((i) => i.start + i.length)), segment: 0, effects: [], items, warnings: [] };
}

const timeline: Timeline = {
  mob: 1,
  mobId: "",
  name: "t",
  kind: "composition",
  slots: [],
  partial: false,
  warnings: [],
  timecode: null,
  tracks: [
    track("timecode", [25, 1], [item(1, "timecode", 0, 100)]),
    track("picture", [25, 1], [item(2, "sourceClip", 0, 50), item(3, "transition", 40, 10), item(4, "sourceClip", 40, 60)]),
    track("sound", [48000, 1], [item(5, "sourceClip", 0, 192000)]),
    track("descriptiveMetadata", [25, 1], [item(6, "marker", 30, 0)], "event"),
  ],
};

describe("timeline layout", () => {
  it("labels rows and converts rates to the picture track's base rate", () => {
    const l = layout(timeline);
    expect(l.rows.map((r) => r.label)).toEqual(["V1", "A1", "M1", "TC1"]);
    expect(l.baseRate).toEqual({ num: 25, den: 1 });
    expect(l.rows[1]!.scale).toBeCloseTo(25 / 48000);
    expect(l.duration).toBeCloseTo(100);
    expect(l.rows[0]!.y).toBe(RULER_HEIGHT);
  });

  it("chooses readable tick steps", () => {
    expect(tickStep(100, 25)).toBe(1);
    expect(tickStep(10, 25)).toBe(10);
    expect(tickStep(1, 25)).toBe(125);
    expect(tickStep(0.001, 25)).toBe(90000);
  });

  it("hit-tests transitions and markers above clips", () => {
    const l = layout(timeline);
    const picture = l.rows[0]!;
    const at = (frame: number, row = picture) => hitTest(l, 100 + frame * 10, row.y + 5, 10, 0, 100)?.item.object;
    expect(at(10)).toBe(2);
    expect(at(45)).toBe(3);
    expect(at(70)).toBe(4);
    expect(at(30, l.rows[2])).toBe(6);
    expect(hitTest(l, 50, picture.y + 5, 10, 0, 100)).toBeNull();
  });
});

describe("visible ranges", () => {
  const sequence = track("picture", [25, 1], [item(1, "sourceClip", 0, 10), item(2, "transition", 8, 4), item(3, "sourceClip", 10, 10), item(4, "filler", 20, 5), item(5, "sourceClip", 25, 10)]);
  const row = layout({ ...timeline, tracks: [sequence] }).rows[0]!;

  it("covers exactly the items overlapping a window", () => {
    expect(visibleRange(row, 0, 5)).toEqual([0, 1]);
    expect(visibleRange(row, 12, 18)).toEqual([1, 3]);
    expect(visibleRange(row, 21, 26)).toEqual([3, 5]);
    expect(visibleRange(row, 40, 50)).toEqual([5, 5]);
  });

  it("matches a linear scan for random windows, sorted or not", () => {
    const shuffled = track("sound", [48000, 1], [item(1, "sourceClip", 50, 10), item(2, "sourceClip", 0, 100), item(3, "marker", 20, 0), item(4, "sourceClip", 70, 5)]);
    for (const t of [sequence, shuffled]) {
      const r = layout({ ...timeline, tracks: [t] }).rows[0]!;
      for (let n = 0; n < 200; n++) {
        const from = Math.random() * 120 * r.scale;
        const to = from + Math.random() * 30 * r.scale;
        const [first, last] = visibleRange(r, from, to);
        t.items.forEach((it, i) => {
          const overlaps = (it.start + it.length) * r.scale >= from && it.start * r.scale <= to;
          if (overlaps) expect(i >= first && i < last).toBe(true);
        });
      }
    }
    expect(layout({ ...timeline, tracks: [shuffled] }).rows[0]!.sorted).toBe(false);
  });
});

describe("RunMerger", () => {
  it("joins touching rectangles of one colour and keeps others apart", () => {
    const drawn: [number, number, string][] = [];
    const merger = new RunMerger((l, w, c) => drawn.push([l, w, c]));
    merger.add(0, 0.2, "a");
    merger.add(0.2, 0.3, "a");
    merger.add(0.9, 0.1, "a");
    merger.add(1.0, 0.1, "b");
    merger.add(5, 0.1, "b");
    merger.flush();
    merger.flush();
    expect(drawn).toEqual([
      [0, 1, "a"],
      [1.0, 1, "b"],
      [5, 1, "b"],
    ]);
  });
});

describe("clips inside effects", () => {
  const plain = item(1, "sourceClip", 0, 10);
  const wrapped = { ...item(2, "operationGroup", 10, 10), clip: 5, effects: [{ object: 2, name: "Audio Gain" }, { object: 3, name: "Pan" }] };

  it("hold their clip and effects for selection", () => {
    expect(holds(wrapped, 2)).toBe(true);
    expect(holds(wrapped, 3)).toBe(true);
    expect(holds(wrapped, 5)).toBe(true);
    expect(holds(wrapped, 1)).toBe(false);
    expect(holds(wrapped, null)).toBe(false);
    expect(holds(plain, 1)).toBe(true);
  });

  it("select the clip, not the outer effect", () => {
    expect(selectionTarget(wrapped)).toBe(5);
    expect(selectionTarget(plain)).toBe(1);
  });

  it("name their effects in the badge when there is room", () => {
    const measure = (text: string) => text.length * 6;
    expect(badgeText(wrapped, 200, measure)).toBe("fx Audio Gain, Pan");
    expect(badgeText(wrapped, 50, measure)).toBe("fx");
    expect(badgeText(plain, 200, measure)).toBeNull();
  });
});

describe("channelFormat", () => {
  it("names the common audio formats", () => {
    expect(channelFormat(undefined)).toBe("");
    expect(channelFormat(0)).toBe("");
    expect(channelFormat(1)).toBe("Mono");
    expect(channelFormat(2)).toBe("Stereo");
    expect(channelFormat(6)).toBe("5.1");
    expect(channelFormat(8)).toBe("7.1");
    expect(channelFormat(4)).toBe("4 ch");
  });
});
