# aaftool

`aaftool` is the command-line companion to the editor. Every command exits with 0 on success, 1 on failure (for
`validate`, when the file has errors) and 2 on a usage error.

## Inspecting

| Command | Output |
|---|---|
| `aaftool validate <file> [--json]` | Diagnostics (errors, warnings and notes) and a summary line. Exits with 1 if there are errors. |
| `aaftool dump <file> [--json] [--depth N] [--header\|--metadict]` | The object tree from the root, the Header or the MetaDictionary. `--json` uses the canonical form in SPEC §8.1. |
| `aaftool timeline <file> --mobs` | One line per mob: object ID, kind, MobID, number of tracks and name. Top-level compositions are marked. |
| `aaftool timeline <file> [--mob NAME\|ID\|MOBID] [--json]` | A mob's tracks and their items, with positions in edit units. Defaults to the first composition. |
| `aaftool cfb <file>` | The raw compound-file directory: storages, streams, sizes and CLSIDs. |

## Saving

| Command | Effect |
|---|---|
| `aaftool roundtrip <in> <out> [--v3\|--v4] [--regenerate-layout]` | Loads and saves without changes. By default the output is identical to the source in structure and stream bytes. `--v3` and `--v4` choose 512- or 4096-byte sectors, and `--regenerate-layout` renames the internal storages. |
| `aaftool cfb-roundtrip <in> <out> [--v3\|--v4]` | Rewrites only the compound-file container, without interpreting the AAF objects. |

## Embedded media

| Command | Effect |
|---|---|
| `aaftool extract <file> --list` | Lists each embedded essence stream: index, MobID, size and the name of the mob it belongs to. |
| `aaftool extract <file> <mobid\|index> <out>` | Writes one essence stream to a file, streaming in 1 MiB chunks. |
| `aaftool set-essence <in> <mobid\|index> <data> <out>` | Replaces an essence stream with a file's contents and saves. The data is copied while saving, never loaded into memory. |

## Scripting edits

`aaftool rpc <file> (--call METHOD PARAMS)... [--save OUT]` runs editor RPC calls in order (SPEC §8.3). It prints
each result as JSON and optionally saves the result. The calls are the same ones the editor makes, so every edit is
validated, and a refused edit stops the run with an error.

```
# Split the clip under frame 100 on a track (object 42), then add a marker, and save.
aaftool rpc in.aaf \
  --call timeline.op '{"op":"split","slot":42,"position":100}' \
  --call timeline.op '{"op":"addMarker","mob":7,"position":100,"comment":"check"}' \
  --save out.aaf

# Relink media from one volume to another.
aaftool rpc in.aaf --call timeline.op '{"op":"relink","find":"file:///Volumes/Old/","replace":"file:///Volumes/New/"}' --save out.aaf

# Rename a mob (find the property's pid with: --call object.get '{"id":7}').
aaftool rpc in.aaf --call object.setProperty '{"id":7,"pid":17410,"value":{"t":"string","v":"New name"}}' --save out.aaf
```

Useful methods:

- `timeline.mobs`, `timeline.get {mob}` and `timeline.resolve {clip}`;
- `timeline.op` with `split`, `lift`, `rippleDelete`, `trim`, `move`, `insertClip`, `overwriteClip`, `addTrack`, `removeTrack`, `addMarker` or `relink`;
- `object.get`, `object.setProperty`, `object.create` and `object.delete`;
- `search.query` and `doc.validate`.
