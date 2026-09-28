import { describe, expect, it } from "vitest";

import { expandTimeline, type WireTimeline } from "./timelineWire";

const wire: WireTimeline = {
  mob: 1,
  mobId: "m",
  name: "T",
  kind: "composition",
  slots: [10],
  partial: false,
  warnings: [],
  timecode: null,
  sources: [{ mobId: "urn:a", mob: 7, mobName: "Clip A", mobKind: "master", original: false }],
  tracks: [
    {
      slot: 10,
      slotId: 1,
      name: "",
      physicalNumber: 1,
      kind: "picture",
      slotKind: "timeline",
      editRate: { num: 25, den: 1 },
      origin: 0,
      length: 30,
      segment: 11,
      effects: [],
      warnings: [],
      items: [
        { object: 20, kind: "sourceClip", start: 0, length: 10, source: { ref: 0, slotId: 1, startTime: 5 } },
        { object: 21, kind: "filler", start: 10, length: 5, label: "Filler" },
        { object: 22, kind: "operationGroup", start: 15, length: 15, label: "Dissolve", effect: "Dissolve", nested: [[{ object: 23, kind: "sourceClip", start: 0, length: 15, class: "SourceClip", label: "Renamed", source: { ref: 0, slotId: 2, startTime: 9 } }]] },
        { object: 24, kind: "marker", start: 3, length: 0, class: "DescriptiveMarker", hasLength: false, label: "note", comment: "note" },
      ],
    },
  ],
};

describe("expandTimeline", () => {
  it("restores sources, labels, classes and hasLength", () => {
    const t = expandTimeline(wire);
    expect("sources" in t).toBe(false);
    const [clip, filler, effect, marker] = t.tracks[0]!.items;
    expect(clip).toEqual({ object: 20, kind: "sourceClip", class: "SourceClip", start: 0, length: 10, hasLength: true, label: "Clip A", source: { mobId: "urn:a", mob: 7, mobName: "Clip A", mobKind: "master", original: false, slotId: 1, startTime: 5 } });
    expect(filler).toEqual({ object: 21, kind: "filler", class: "Filler", start: 10, length: 5, hasLength: true, label: "Filler" });
    expect(effect!.nested![0]![0]!.label).toBe("Renamed");
    expect(effect!.nested![0]![0]!.source!.startTime).toBe(9);
    expect(marker!.class).toBe("DescriptiveMarker");
    expect(marker!.hasLength).toBe(false);
  });

  it("rejects a reference to a missing source", () => {
    expect(() => expandTimeline({ ...wire, sources: [] })).toThrow("missing");
  });
});
