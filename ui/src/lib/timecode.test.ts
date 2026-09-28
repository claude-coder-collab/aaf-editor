import { describe, expect, it } from "vitest";

import { formatTimecode, nominalFps, parseTimecode } from "./timecode";

describe("timecode", () => {
  it("formats non-drop timecode", () => {
    expect(formatTimecode(0, 25)).toBe("00:00:00:00");
    expect(formatTimecode(25 * 3600 + 25 * 61 + 3, 25)).toBe("01:01:01:03");
    expect(formatTimecode(-24, 24)).toBe("-00:00:01:00");
    expect(formatTimecode(10, 0)).toBe("--:--:--:--");
  });

  it("formats and parses drop-frame timecode at 29.97", () => {
    expect(formatTimecode(1799, 30, true)).toBe("00:00:59;29");
    expect(formatTimecode(1800, 30, true)).toBe("00:01:00;02");
    expect(formatTimecode(17982, 30, true)).toBe("00:10:00;00");
    expect(formatTimecode(107892, 30, true)).toBe("01:00:00;00");
    for (const frames of [0, 1799, 1800, 17981, 17982, 107891, 107892, 123456]) {
      expect(parseTimecode(formatTimecode(frames, 30, true), 30, true)).toBe(frames);
    }
  });

  it("round-trips non-drop and rejects malformed input", () => {
    for (const frames of [0, 1, 24, 86399]) {
      expect(parseTimecode(formatTimecode(frames, 24), 24)).toBe(frames);
    }
    expect(parseTimecode("00:00:00:25", 25)).toBeNull();
    expect(parseTimecode("nonsense", 25)).toBeNull();
    expect(nominalFps(30000, 1001)).toBe(30);
    expect(nominalFps(24, 1)).toBe(24);
  });
});
