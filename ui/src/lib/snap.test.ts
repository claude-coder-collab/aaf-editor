import { describe, expect, it } from "vitest";

import { dragZone, snap } from "./snap";

describe("snap", () => {
  it("snaps to the nearest point within tolerance", () => {
    expect(snap(10.4, [0, 10, 20], 1)).toBe(10);
    expect(snap(14, [0, 10, 20], 1)).toBe(14);
    expect(snap(19.5, [10, 20, 19], 1)).toBe(19);
  });
});

describe("dragZone", () => {
  it("detects edges and scales them for small items", () => {
    expect(dragZone(102, 100, 100)).toBe("head");
    expect(dragZone(197, 100, 100)).toBe("tail");
    expect(dragZone(150, 100, 100)).toBe("body");
    expect(dragZone(101, 100, 8)).toBe("head");
    expect(dragZone(104, 100, 8)).toBe("body");
  });
});
