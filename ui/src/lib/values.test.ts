import { describe, expect, it } from "vitest";

import { defaultValue, editorKind, formatValue, parseScalar, withField, withItem, type Types } from "./values";

const types: Types = {
  u8: { id: "u8", name: "aafUInt8", kind: "integer", size: 1, signed: false },
  i32: { id: "i32", name: "aafInt32", kind: "integer", size: 4, signed: true },
  i64: { id: "i64", name: "aafInt64", kind: "integer", size: 8, signed: true },
  len: { id: "len", name: "aafLengthType", kind: "rename", element: "i64" },
  str: { id: "str", name: "aafString", kind: "string", element: "chr" },
  "01040100-0000-0000-060e-2b3401040101": { id: "01040100-0000-0000-060e-2b3401040101", name: "Boolean", kind: "enumeration", element: "u8" },
  rate: { id: "rate", name: "Rational", kind: "record", fields: [{ name: "Numerator", type: "i32" }, { name: "Denominator", type: "i32" }] },
  kind: { id: "kind", name: "Kind", kind: "enumeration", element: "u8", elements: [{ name: "A", value: "0" }, { name: "B", value: "1" }] },
  arr: { id: "arr", name: "Ints", kind: "var_array", element: "i32" },
};

describe("editorKind", () => {
  it("follows renames and recognises special types", () => {
    expect(editorKind(types, "len")).toBe("int");
    expect(editorKind(types, "u8")).toBe("uint");
    expect(editorKind(types, "01040100-0000-0000-060e-2b3401040101")).toBe("bool");
    expect(editorKind(types, "rate")).toBe("record");
    expect(editorKind(types, "missing")).toBe("readonly");
  });
});

describe("parseScalar", () => {
  it("checks integer ranges using the type size", () => {
    expect(parseScalar("uint", "255", types.u8)).toEqual({ t: "uint", v: "255" });
    expect(parseScalar("uint", "256", types.u8)).toMatch(/between/);
    expect(parseScalar("int", "-9223372036854775808", types.i64)).toEqual({ t: "int", v: "-9223372036854775808" });
    expect(parseScalar("int", "1.5", types.i64)).toMatch(/whole number/);
  });
  it("validates identifiers", () => {
    expect(parseScalar("auid", "0D010101-0101-2F00-060E-2B3402060101")).toEqual({ t: "auid", v: "0d010101-0101-2f00-060e-2b3402060101" });
    expect(parseScalar("auid", "nope")).toMatch(/Expected/);
    expect(parseScalar("mobid", "urn:smpte:umid:060a2b34.01010105.01010f20.13000000.ea07beca.13fb4df7.b7b1eae3.e902adb7")).toHaveProperty("t", "mobid");
  });
});

describe("defaultValue and formatting", () => {
  it("builds defaults like the server", () => {
    const rate = defaultValue(types, "rate");
    expect(formatValue(rate!)).toBe("0/1");
    expect(defaultValue(types, "kind")).toEqual({ t: "enum", v: "0", name: "A" });
    expect(defaultValue(types, "arr")).toEqual({ t: "array", items: [] });
    expect(defaultValue(types, "missing")).toBeNull();
  });
  it("updates records and arrays immutably", () => {
    const rate = defaultValue(types, "rate")!;
    const changed = withField(rate, "Numerator", { t: "int", v: "25" });
    expect(formatValue(changed)).toBe("25/1");
    expect(formatValue(rate)).toBe("0/1");
    const list = { t: "array" as const, items: [{ t: "int" as const, v: "1" }] };
    expect(formatValue(withItem(list, 0, { t: "int", v: "2" }, true))).toBe("[2, 1]");
    expect(formatValue(withItem(list, 0, null))).toBe("[]");
  });
  it("formats long byte strings compactly", () => {
    expect(formatValue({ t: "bytes", v: "ab".repeat(100) })).toMatch(/100 bytes/);
    expect(formatValue({ t: "enum", v: "3", name: "" })).toBe("3");
  });
});

