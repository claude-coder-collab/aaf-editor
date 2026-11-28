import { describe, expect, it } from "vitest";

import { sampleAccurateEditSummary } from "./batch";

const changes = { objects: [], created: [], properties: [], referencedPropertiesChanged: false };

describe("sampleAccurateEditSummary", () => {
  it("reports nothing found", () => {
    expect(sampleAccurateEditSummary({ changes, cuts: 0, silenced: 0, mobsDeleted: 0, essenceDeleted: 0 })).toBe("No sample-accurate edits found.");
  });

  it("counts cuts, silenced frames and deleted mobs", () => {
    expect(sampleAccurateEditSummary({ changes, cuts: 10, silenced: 7, mobsDeleted: 180, essenceDeleted: 60 })).toBe(
      "Removed 17 sample-accurate edits (10 cuts, 7 silenced); deleted 180 unused mobs.",
    );
    expect(sampleAccurateEditSummary({ changes, cuts: 1, silenced: 0, mobsDeleted: 0, essenceDeleted: 0 })).toBe("Removed 1 sample-accurate edit (1 cut).");
  });

  it("reports mobs deleted when no clips were left", () => {
    expect(sampleAccurateEditSummary({ changes, cuts: 0, silenced: 0, mobsDeleted: 3, essenceDeleted: 0 })).toBe("Deleted 3 unused mobs.");
  });
});
