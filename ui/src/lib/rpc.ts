import { expandTimeline, type WireTimeline } from "./timelineWire";

export type TaggedValue =
  | { t: "null" }
  | { t: "bool"; v: boolean }
  | { t: "int"; v: string }
  | { t: "uint"; v: string }
  | { t: "string"; v: string }
  | { t: "auid"; v: string; name?: string }
  | { t: "mobid"; v: string }
  | { t: "enum"; v: string; name: string }
  | { t: "extenum"; v: string; name: string }
  | { t: "record"; fields: { name: string; value: TaggedValue }[] }
  | { t: "array"; items: TaggedValue[] }
  | { t: "indirect"; type: string; value: TaggedValue }
  | { t: "opaque"; type: string; bytes: string }
  | { t: "bytes"; v: string };

export interface TypeDescriptor {
  id: string;
  name: string;
  kind: string;
  element?: string;
  className?: string;
  size?: number;
  signed?: boolean;
  count?: number;
  fields?: { name: string; type: string }[];
  elements?: { name: string; value: string }[];
}

export interface WeakTarget {
  id: number | null;
  key: string;
  label: string;
  resolved?: "builtin" | "missing";
}

export interface ImplicitTarget {
  id: number | null;
  status: "resolved" | "null" | "external" | "builtin";
  label?: string;
  class?: string;
}

export interface Referrer {
  id: number;
  class: string;
  label: string;
  pid: number;
  property: string;
  weak: boolean;
}

export interface PropertyInfo {
  pid: number;
  name: string;
  kind: string;
  storedForm: number;
  type?: string;
  optional?: boolean;
  uniqueId?: boolean;
  value?: TaggedValue;
  error?: string;
  children?: { id: number; class: string; label: string }[];
  count?: number;
  target?: WeakTarget;
  targets?: WeakTarget[];
  refers?: ImplicitTarget;
  referrers?: number;
  size?: number;
}

export interface AvailableProperty {
  pid: number;
  name: string;
  type: string;
  optional: boolean;
  kind: string;
}

export interface ObjectInfo {
  id: number;
  class: string;
  classId: string;
  concrete: boolean;
  label: string;
  parent: number | null;
  parentPid: number;
  attached: boolean;
  properties: PropertyInfo[];
  available: AvailableProperty[];
  types: Record<string, TypeDescriptor>;
  referencedBy?: Referrer[];
  referencedByCount?: number;
}

export interface TreeItem {
  id: number;
  class: string;
  label: string;
  pid: number;
  property: string;
  index: number;
  key: string;
  childCount: number;
}

export interface DocInfo {
  open: boolean;
  path?: string;
  name?: string;
  dirty?: boolean;
  canUndo?: boolean;
  canRedo?: boolean;
  undo?: string | null;
  redo?: string | null;
  objectCount?: number;
  version?: number;
  header?: number | null;
  metaDictionary?: number | null;
}

export interface ChangeSet {
  objects: number[];
  created: number[];
  properties: { object: number; pid: number }[];
  referencedPropertiesChanged: boolean;
}

export interface Diagnostic {
  severity: "info" | "warning" | "error";
  object: number | null;
  pid: number;
  property: string;
  message: string;
}

export interface LabelInfo {
  auid: string;
  name: string;
  deprecated: boolean;
}

export interface SearchHit {
  id: number;
  class: string;
  label: string;
  value?: TaggedValue;
}

export interface EditRate {
  num: number;
  den: number;
}

export interface TimelineItem {
  object: number;
  kind: string;
  class: string;
  start: number;
  length: number;
  hasLength: boolean;
  label: string;
  effect?: string;
  comment?: string;
  source?: { mobId: string; slotId: number; startTime: number; mob: number | null; mobName: string; mobKind: string; original: boolean };
  timecode?: { start: number; fps: number; drop: boolean };
  nested?: TimelineItem[][];
  /// For an effect whose only input is a source clip (possibly through further such effects): that clip. The item
  /// then carries the clip's label and source.
  clip?: number;
  /// The effects around `clip`, from the track inwards.
  effects?: { object: number; name: string }[];
}

export interface TimelineTrack {
  slot: number;
  slotId: number;
  name: string;
  physicalNumber: number | null;
  kind: string;
  slotKind: string;
  editRate: EditRate;
  origin: number;
  length: number;
  segment: number;
  effects: { object: number; name: string }[];
  items: TimelineItem[];
  warnings: string[];
}

export interface Timeline {
  mob: number;
  mobId: string;
  name: string;
  kind: string;
  /// Every track, or only the changed ones when `partial` is set.
  tracks: TimelineTrack[];
  /// Every slot of the mob, in order.
  slots: number[];
  partial: boolean;
  warnings: string[];
  timecode: { start: number; fps: number; drop: boolean } | null;
}

export interface MobSummary {
  id: number;
  mobId: string;
  name: string;
  kind: string;
  tracks: number;
  topLevel: boolean;
}

export interface SourceChain {
  status: string;
  links: { mob: number | null; mobId: string; name: string; kind: string; slotId: number; position: number; editRate: EditRate; descriptor: string }[];
  essence: { embedded: boolean; essenceData: number | null; locators: string[]; descriptor: string } | null;
}

export class RpcError extends Error {
  constructor(
    message: string,
    readonly code: number,
  ) {
    super(message);
  }
}

/// Sends one JSON-RPC request text and resolves with the response object.
export type Transport = (request: string) => Promise<unknown>;
export type EventHandler = (method: string, params: unknown) => void;

interface RpcResponse {
  id: number;
  result?: unknown;
  error?: { code: number; message: string };
}

declare global {
  interface Window {
    aafRpc?: (request: string) => Promise<unknown>;
    aafHost?: (command: string, args: unknown) => Promise<unknown>;
    __aafEvent?: (event: { method: string; params: unknown }) => void;
    __aafSmoke?: { file: string };
    __aafTest?: boolean;
    __aafTimeline?: {
      tracks: () => { slot: number; label: string; kind: string; items: { object: number; kind: string; start: number; length: number; clip?: number; effects?: string[] }[] }[];
      itemRect: (object: number) => { x: number; y: number; width: number; height: number } | null;
      measure: (repeat: number) => { draw: number; pick: number };
      zoom: (factor: number) => void;
      show: (object: number, pixels: number) => void;
    };
  }
}

export class RpcClient {
  private nextId = 0;
  private readonly handlers = new Set<EventHandler>();

  constructor(private readonly transport: Transport) {}

  async call<T>(method: string, params: Record<string, unknown> = {}): Promise<T> {
    const id = ++this.nextId;
    const raw = await this.transport(JSON.stringify({ jsonrpc: "2.0", id, method, params }));
    const response = (typeof raw === "string" ? JSON.parse(raw) : raw) as RpcResponse;
    if (response.error) {
      throw new RpcError(response.error.message, response.error.code);
    }
    return response.result as T;
  }

  onEvent(handler: EventHandler): () => void {
    this.handlers.add(handler);
    return () => this.handlers.delete(handler);
  }

  dispatch(method: string, params: unknown): void {
    for (const handler of this.handlers) {
      handler(method, params);
    }
  }

  docInfo = () => this.call<DocInfo>("doc.info");
  open = (path: string) => this.call<DocInfo>("doc.open", { path });
  save = () => this.call<DocInfo>("doc.save");
  saveAs = (path: string) => this.call<DocInfo>("doc.saveAs", { path });
  validate = () => this.call<Diagnostic[]>("doc.validate");
  children = (id: number, offset = 0, limit = 200) => this.call<{ total: number; items: TreeItem[] }>("tree.children", { id, offset, limit });
  path = (id: number) => this.call<number[]>("tree.path", { id });
  object = (id: number) => this.call<ObjectInfo>("object.get", { id });
  setProperty = (id: number, pid: number, value: TaggedValue, updateReferences = false) =>
    this.call<ChangeSet>("object.setProperty", { id, pid, value, updateReferences });
  removeProperty = (id: number, pid: number) => this.call<ChangeSet>("object.removeProperty", { id, pid });
  create = (parent: number, pid: number, cls: string, index?: number) =>
    this.call<{ id: number; changes: ChangeSet }>("object.create", { parent, pid, class: cls, index });
  remove = (id: number, force = false) => this.call<ChangeSet>("object.delete", { id, force });
  move = (parent: number, pid: number, from: number, to: number) => this.call<ChangeSet>("object.move", { parent, pid, from, to });
  setWeakRef = (id: number, pid: number, target: number) => this.call<ChangeSet>("object.setWeakRef", { id, pid, target });
  setReference = (id: number, pid: number, target: number) => this.call<ChangeSet>("object.setReference", { id, pid, target });
  candidates = (id: number, pid: number) => this.call<SearchHit[]>("object.candidates", { id, pid });
  labelFamily = (auid: string) => this.call<LabelInfo[]>("labels.family", { auid });
  subclasses = (cls: string) => this.call<{ name: string; id: string }[]>("model.subclasses", { class: cls });
  undo = () => this.call<ChangeSet>("edit.undo");
  redo = () => this.call<ChangeSet>("edit.redo");
  history = () => this.call<{ items: string[]; position: number }>("edit.history");
  search = (text: string, cls = "", limit = 200) => this.call<SearchHit[]>("search.query", { text, class: cls, limit });
  mobs = () => this.call<MobSummary[]>("timeline.mobs");
  timelineOp = (params: Record<string, unknown>) => this.call<{ changes: ChangeSet; id?: number; count?: number }>("timeline.op", params);
  timeline = async (mob: number, changed?: number[]) => expandTimeline(await this.call<WireTimeline>("timeline.get", changed ? { mob, changed } : { mob }));
  resolve = (clip: number) => this.call<SourceChain>("timeline.resolve", { clip });
  extractEssence = (id: number, path: string) => this.call<{ size: number }>("essence.extract", { id, path });
  replaceEssence = (id: number, path: string) => this.call<ChangeSet>("essence.replace", { id, path });
}

/// Connects to the webview host's bound functions, or returns null when running outside the editor.
export function hostTransport(): Transport | null {
  const bound = window.aafRpc;
  return bound ? (request: string) => bound(request) : null;
}

export async function hostCommand<T>(command: string, args: Record<string, unknown> = {}): Promise<T | null> {
  if (!window.aafHost) {
    return null;
  }
  return (await window.aafHost(command, args)) as T;
}
