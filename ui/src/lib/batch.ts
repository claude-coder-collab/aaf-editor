import type { RpcClient, SampleAccurateEditReport } from "./rpc";

export interface BatchProcess {
  id: string;
  label: string;
  description: string;
  /// Runs the process as one undoable step and returns a summary for the message bar.
  run: (client: RpcClient) => Promise<string>;
}

const plural = (n: number, one: string, many = `${one}s`) => `${n.toLocaleString()} ${n === 1 ? one : many}`;

export function sampleAccurateEditSummary(r: SampleAccurateEditReport): string {
  const total = r.cuts + r.silenced;
  const mobs = r.mobsDeleted > 0 ? plural(r.mobsDeleted, "unused mob") : "";
  if (total === 0) return mobs ? `Deleted ${mobs}.` : "No sample-accurate edits found.";
  const how = [r.cuts > 0 ? plural(r.cuts, "cut") : "", r.silenced > 0 ? `${r.silenced.toLocaleString()} silenced` : ""].filter(Boolean).join(", ");
  return `Removed ${plural(total, "sample-accurate edit")} (${how})${mobs ? `; deleted ${mobs}` : ""}.`;
}

export const batchProcesses: BatchProcess[] = [
  {
    id: "removeSampleAccurateEdits",
    label: "Remove sample-accurate edits",
    description:
      "Removes every Pro Tools “Sample accurate edit” clip from every composition. Frames next to a clip become a cut using its media handle; the rest become silence. Then deletes the master clips and sources nothing uses any more.",
    run: async (client) => sampleAccurateEditSummary(await client.removeSampleAccurateEdits()),
  },
];
