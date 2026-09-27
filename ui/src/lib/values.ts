import type { TaggedValue, TypeDescriptor } from "./rpc";

export type Types = Record<string, TypeDescriptor>;

const AUID_TYPE = "01030100-0000-0000-060e-2b3401040101";
const MOBID_TYPE = "01030200-0000-0000-060e-2b3401040101";
const BOOLEAN_TYPE = "01040100-0000-0000-060e-2b3401040101";

/// Follows renames to the underlying type.
export function resolveType(types: Types, id: string | undefined): TypeDescriptor | undefined {
  let type = id ? types[id] : undefined;
  for (let depth = 0; type?.kind === "rename" && depth < 16; ++depth) {
    type = type.element ? types[type.element] : undefined;
  }
  return type;
}

export type EditorKind = "bool" | "int" | "uint" | "string" | "enum" | "extenum" | "auid" | "mobid" | "record" | "array" | "readonly";

export function editorKind(types: Types, typeId: string | undefined): EditorKind {
  const type = resolveType(types, typeId);
  if (!type) {
    return "readonly";
  }
  switch (type.kind) {
    case "integer":
      return type.signed ? "int" : "uint";
    case "generic_character":
      return "uint";
    case "string":
    case "character":
      return "string";
    case "enumeration":
      return type.id === BOOLEAN_TYPE ? "bool" : "enum";
    case "ext_enum":
      return "extenum";
    case "record":
      if (type.id === AUID_TYPE) return "auid";
      if (type.id === MOBID_TYPE) return "mobid";
      return "record";
    case "fixed_array":
    case "var_array":
    case "set":
      return "array";
    default:
      return "readonly";
  }
}

export function formatValue(value: TaggedValue | undefined): string {
  if (!value) {
    return "";
  }
  switch (value.t) {
    case "null":
      return "null";
    case "bool":
      return value.v ? "true" : "false";
    case "int":
    case "uint":
    case "string":
    case "auid":
    case "mobid":
      return value.v;
    case "enum":
      return value.name || value.v;
    case "extenum":
      return value.name || value.v;
    case "record":
      if (value.fields.length === 2 && value.fields[0]?.name === "Numerator" && value.fields[1]?.name === "Denominator") {
        return `${formatValue(value.fields[0].value)}/${formatValue(value.fields[1].value)}`;
      }
      return `{${value.fields.map((f) => `${f.name}: ${formatValue(f.value)}`).join(", ")}}`;
    case "array":
      return `[${value.items.map(formatValue).join(", ")}]`;
    case "indirect":
      return formatValue(value.value);
    case "opaque":
      return `opaque ${value.type} (${value.bytes.length / 2} bytes)`;
    case "bytes":
      return value.v.length > 64 ? `${value.v.slice(0, 64)}… (${value.v.length / 2} bytes)` : value.v;
  }
}

const AUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const MOBID_RE = /^(urn:smpte:umid:)?([0-9a-f]{8}\.?){8}$/i;

/// Parses text typed into a scalar editor. Returns the value or an error message.
export function parseScalar(kind: EditorKind, text: string, type?: TypeDescriptor): TaggedValue | string {
  const trimmed = text.trim();
  switch (kind) {
    case "string":
      return { t: "string", v: text };
    case "int":
    case "uint": {
      if (!/^-?\d+$/.test(trimmed)) {
        return "Enter a whole number";
      }
      const n = BigInt(trimmed);
      const bits = BigInt((type?.size ?? 8) * 8);
      const min = kind === "int" ? -(1n << (bits - 1n)) : 0n;
      const max = kind === "int" ? (1n << (bits - 1n)) - 1n : (1n << bits) - 1n;
      if (n < min || n > max) {
        return `Must be between ${min} and ${max}`;
      }
      return { t: kind, v: n.toString() };
    }
    case "auid":
      return AUID_RE.test(trimmed) ? { t: "auid", v: trimmed.toLowerCase() } : "Expected xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";
    case "mobid":
      return MOBID_RE.test(trimmed) ? { t: "mobid", v: trimmed.toLowerCase() } : "Expected urn:smpte:umid:… (64 hex digits)";
    default:
      return "This value cannot be edited as text";
  }
}

/// A starting value for a newly added property, mirroring the server's defaults.
export function defaultValue(types: Types, typeId: string | undefined, depth = 0): TaggedValue | null {
  const type = resolveType(types, typeId);
  if (!type || depth > 16) {
    return null;
  }
  switch (editorKind(types, type.id)) {
    case "bool":
      return { t: "bool", v: false };
    case "int":
      return { t: "int", v: "0" };
    case "uint":
      return { t: "uint", v: "0" };
    case "string":
      return { t: "string", v: type.kind === "character" ? " " : "" };
    case "enum": {
      const first = type.elements?.[0];
      return { t: "enum", v: first?.value ?? "0", name: first?.name ?? "" };
    }
    case "extenum": {
      const first = type.elements?.[0];
      return first ? { t: "extenum", v: first.value, name: first.name } : { t: "auid", v: "00000000-0000-0000-0000-000000000000" };
    }
    case "auid":
      return { t: "auid", v: "00000000-0000-0000-0000-000000000000" };
    case "mobid":
      return { t: "mobid", v: "urn:smpte:umid:" + Array(8).fill("00000000").join(".") };
    case "record": {
      const fields: { name: string; value: TaggedValue }[] = [];
      for (const field of type.fields ?? []) {
        let value = defaultValue(types, field.type, depth + 1);
        if (!value) return null;
        if (field.name === "Denominator" && (value.t === "int" || value.t === "uint")) value = { ...value, v: "1" };
        fields.push({ name: field.name, value });
      }
      return { t: "record", fields };
    }
    case "array": {
      if (type.kind !== "fixed_array") return { t: "array", items: [] };
      const items: TaggedValue[] = [];
      for (let i = 0; i < (type.count ?? 0); ++i) {
        const item = defaultValue(types, type.element, depth + 1);
        if (!item) return null;
        items.push(item);
      }
      return { t: "array", items };
    }
    default:
      return null;
  }
}

/// Replaces field `name` of a record value.
export function withField(record: TaggedValue, name: string, value: TaggedValue): TaggedValue {
  if (record.t !== "record") {
    return record;
  }
  return { t: "record", fields: record.fields.map((f) => (f.name === name ? { name, value } : f)) };
}

/// Replaces, inserts (`value` at `index` with `insert`) or removes (`value` null) an array element.
export function withItem(array: TaggedValue, index: number, value: TaggedValue | null, insert = false): TaggedValue {
  if (array.t !== "array") {
    return array;
  }
  const items = [...array.items];
  if (value === null) {
    items.splice(index, 1);
  } else if (insert) {
    items.splice(index, 0, value);
  } else {
    items[index] = value;
  }
  return { t: "array", items };
}
