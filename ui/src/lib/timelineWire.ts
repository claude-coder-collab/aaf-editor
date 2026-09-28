import type { Timeline, TimelineItem, TimelineTrack } from "./rpc";

type Source = NonNullable<TimelineItem["source"]>;
type SharedSource = Omit<Source, "slotId" | "startTime">;

interface WireItem extends Omit<TimelineItem, "class" | "hasLength" | "label" | "source" | "nested"> {
  class?: string;
  hasLength?: boolean;
  label?: string;
  source?: { ref: number; slotId: number; startTime: number };
  nested?: WireItem[][];
}

/// The compact form `timeline.get` sends: sources are listed once, and default classes, labels and hasLength are omitted.
export interface WireTimeline extends Omit<Timeline, "tracks"> {
  sources: SharedSource[];
  tracks: (Omit<TimelineTrack, "items"> & { items: WireItem[] })[];
}

const capitalize = (text: string) => text.charAt(0).toUpperCase() + text.slice(1);

/// Restores the full item shape the UI works with.
export function expandTimeline(wire: WireTimeline): Timeline {
  const expand = (items: WireItem[]): TimelineItem[] =>
    items.map((item) => {
      const shared = item.source ? wire.sources[item.source.ref] : undefined;
      if (item.source && !shared) throw new Error(`timeline source ${item.source.ref} is missing`);
      const out: TimelineItem = {
        ...item,
        class: item.class ?? capitalize(item.kind),
        hasLength: item.hasLength ?? true,
        label: item.label ?? shared?.mobName ?? "",
        source: item.source && shared ? { ...shared, slotId: item.source.slotId, startTime: item.source.startTime } : undefined,
        nested: item.nested?.map(expand),
      };
      if (!out.source) delete out.source;
      if (!out.nested) delete out.nested;
      return out;
    });
  const { sources: _sources, ...rest } = wire;
  return { ...rest, tracks: wire.tracks.map((track) => ({ ...track, items: expand(track.items) })) };
}
