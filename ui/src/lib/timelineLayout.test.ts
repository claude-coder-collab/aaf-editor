import { describe, expect, it } from "vitest";

import type { Timeline, TimelineItem, TimelineTrack } from "./rpc";
import { hitTest, layout, RULER_HEIGHT, tickStep } from "./timelineLayout";

function item(object: number, kind: string, start: number, length: number): TimelineItem {
  return { object, kind, class: kind, start, length, hasLength: true, label: `item ${object}` };
}

function track(kind: string, rate: [number, number], items: TimelineItem[], slotKind = "timeline"): TimelineTrack {
  return { slot: 0, slotId: 1, name: "", physicalNumber: null, kind, slotKind, editRate: { num: rate[0], den: rate[1] }, origin: 0, length: Math.max(0, ...items.map((i) => i.start + i.length)), segment: 0, effects: [], items };
}

const timeline: Timeline = {
  mob: 1,
  mobId: "",
  name: "t",
  kind: "composition",
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
