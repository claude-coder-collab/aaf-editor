# AAF Editor — Specification

Status: draft v0.2 (2026-09-27). Licence: MIT. This document is the source of truth. Update it whenever a design decision changes, so that another engineer or agent can re-implement the project from it alone.

## 1. Goal

A cross-platform desktop application and reusable C++ library for **inspecting and editing AAF (Advanced Authoring Format) files**. It is written from scratch, with no AAF SDK and no pyaaf2 at runtime.

Two synchronised views over one in-memory model:

1. **Inspector**: the raw object graph (objects, properties, typed values). Every property can be viewed and, where legal, edited.
2. **Timeline**: compositions rendered as tracks and clips, with common editorial operations.

An edit in either view is one undoable command that updates both views.

### 1.1 Non-goals (v1)

- Decoding or playing essence (video/audio). Essence bytes are only preserved, extracted or replaced.
- AAF-XML and AAF-KLV (MXF-style) encodings. Only the Structured Storage (CFB) binary encoding is supported.
- Being a full NLE: no rendering, effects preview or media management beyond relinking.
- Creating AAF files for specific vendor profiles (Avid/Pro Tools edit protocol conformance) beyond preserving what exists. Later milestone, see §12.

## 2. Terminology

| Term | Meaning |
|---|---|
| CFB | Microsoft Compound File Binary format ([MS-CFB]), a "filesystem in a file" with storages (dirs) and streams (files). |
| Stored format | How the AAF object model maps onto CFB storages and streams (§5). |
| MetaDictionary | The in-file schema: ClassDefinitions and TypeDefinitions. |
| PID | 16-bit local property identifier, mapped to a 16-byte AUID through the MetaDictionary. |
| AUID | 16-byte identifier (UUID or SMPTE UL, byte-swapped UL form). |
| MobID | 32-byte SMPTE UMID identifying a Mob. |
| Strong ref | Ownership (containment) reference, which forms a tree. |
| Weak ref | Non-owning reference by unique key (for example a MobID or definition AUID). |

## 3. Architecture

```
aaf-editor/
  CMakeLists.txt, CMakePresets.json      Ninja Multi-Config
  libs/
    common/     aaf::Error / aaf::Result (header-only)
    cfb/        libaafcfb     CFB container read/write (no AAF knowledge)
    core/       libaafcore    stored format, metamodel, object graph, validation
    timeline/   libaaftl      timeline projection + editorial operations
    edit/       libaafedit    command/undo engine, document session
  apps/
    aaftool/    CLI: cfb, cfb-roundtrip (M1); dump, validate, roundtrip, json, extract (later)
    editor/     webview host application + RPC bridge
  ui/           TypeScript frontend (Vite), bundled into the editor binary
  tests/        Catch2 unit tests (tests/<lib>/), helpers (tests/support/), fixtures, fuzz targets (tests/fuzz/)
  tools/        Python scripts (fixture generation, spec checks)
  docs/reference/  AAF specification PDFs (local only, gitignored; fetched by tools/fetch_specs.py)
  model/        built-in AAF baseline metamodel data (generated source)
```

Dependency direction: `cfb ← core ← timeline ← edit ← apps`. The libraries have no third-party runtime dependencies. The apps additionally depend on the webview library and nlohmann/json (fetched with CMake `FetchContent`, pinned by tag).

### 3.1 Language and tooling

- C++26 mode (on MSVC, `cxx_std_23`, which CMake maps to `/std:c++latest`, because CMake does not yet know `cxx_std_26` for MSVC; C++ module scanning is off), restricted to features supported by current GCC, Clang and MSVC (in practice mostly C++23: `std::expected`, `std::span`, `std::byte`, `std::format`, ranges, `std::flat_map` only if all three ship it).
- Library errors use `std::expected<T, aaf::Error>`. No exceptions cross library API boundaries. `Error` carries a code, a message and a byte offset/path when relevant.
- CMake ≥ 3.28, Ninja Multi-Config, presets for `gcc`, `clang` and `msvc`.
- Strict warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` in CI; `/W4 /WX` on MSVC).
- clang-format using the `~/.clang-format` style (copied into the repo as `.clang-format`), clang-tidy in CI.
- Sanitizers: ASan/UBSan and TSan in separate debug CI jobs (presets `clang-asan` and `clang-tsan`). libFuzzer targets for the CFB and stored-format readers.
- Tests use Catch2 v3. Python scripts in `tools/` (with type hints and pytest tests in `tools/tests/`) use pyaaf2 for fixture generation and cross-checks, **test-time only**.

## 4. Layer 1: CFB container (`libaafcfb`)

Implements [MS-CFB] v3 (512-byte sectors) and v4 (4096-byte sectors). Status: **implemented (M1)**. The code lives in `libs/cfb/`, with the shared `aaf::Error`/`aaf::Result` in `libs/common/include/aaf/error.hpp`.

### 4.1 Reading

- `Container::open` parses the header, DIFAT (header plus chained DIFAT sectors), FAT, MiniFAT, directory and mini-stream chain up front, and validates all structure. Stream data is read lazily.
- **Input**: comes through the `ByteSource` interface (`size()`, thread-safe positional `read(offset, span)`). Two implementations exist:
  - `FileSource`: positional reads under a mutex. Memory mapping is a possible later optimisation.
  - `MemorySource`: used by tests and fuzzing.
- **Header checks**:
  - signature `D0 CF 11 E0 A1 B1 1A E1` and byte order `0xFFFE`;
  - (major 3, shift 9) or (major 4, shift 12), otherwise `Errc::unsupported`;
  - mini sector shift 6 and cutoff 4096.
  - The **header CLSID** (offset 8) is exposed as `HeaderInfo::clsid`, because AAF stores its file signature there (§5.1).
- **Directory**:
  - Exposed as `DirEntry{id, parent, name (UTF-16), type, clsid, stateBits, creation and modified times, startSector, size, children}`.
  - `children` are listed in tree order, which is sorted order.
  - Slots not reachable from the root are reported as `EntryType::empty`.
  - Lookups: `find(parent, name)` (CFB name comparison) and `findPath("a/b/c")` (UTF-8).
- **Streams**: `openStream(id)` returns a `StreamReader` with random-access `read(offset, span)` and `readAll(maxSize)`.
  - The reader precomputes the absolute file offset of each sector, or of each 64-byte mini sector (a mini sector never spans two regular sectors).
  - It holds a non-owning pointer to the container's source, so it must not outlive the `Container`.
- **Robustness**:
  - Every read is bounds-checked.
  - Sector chains are rejected if they reference sectors that are special or out of range, or if they are longer than the table they index (which means a cycle).
  - The directory tree is walked iteratively, and an entry linked twice (a cycle or shared node) is an error.
  - FAT and DIFAT counts are checked against the file size before any allocation.
  - A stream whose chain is shorter than its size is an error.
- **Leniency**, needed to read real-world files:
  - A chain longer than the stream needs is accepted.
  - A final sector cut short by end of file is zero-filled.
  - The upper 32 bits of v3 stream sizes are ignored.
  - Names are read up to the first NUL.
  - The header CLSID is not required to be zero.
- **Name ordering** (`compareNames`): shorter names sort first, then names are compared by upper-cased UTF-16 code units. `upperCase` covers ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic. That is sufficient for AAF's ASCII names; other code units compare unchanged.

### 4.2 Writing

- `Builder` describes a tree of storages and streams. Node 0 is the root.
  - `addStorage` and `addStream` validate names: 1–31 UTF-16 units, no `/ \ : !` or NUL, and unique among siblings under the CFB comparison.
  - Stream data is either owned bytes or a `SourceStream{container, entryId}`, which is copied lazily in 1 MiB chunks, so essence is never buffered whole.
  - `Builder::fromContainer` copies a whole tree, including CLSIDs, state bits, timestamps and the header CLSID, with every stream referencing its source.
- **Output**: always a **complete new file**, never an in-place modification. `writeFile` uses `writeFileAtomic`:
  - write to `<path>.tmp-<random>`;
  - fsync (`fsync` on POSIX, `_commit` on Windows);
  - rename over the target (`std::filesystem::rename` on POSIX, `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` on Windows);
  - on failure, delete the temporary file and leave the target untouched.
  - Open issue for M3: saving over a file that is still open as the source needs verifying on Windows.
- **Version**: inherited from the source by default (`Builder::setVersion` overrides it). New builders default to v4. v3 streams are limited to 2 GiB.
- **Layout**: sectors are laid out in this order:
  1. regular streams, in directory order;
  2. the mini stream (streams of 1–4095 bytes, 64-byte aligned);
  3. the MiniFAT;
  4. the directory;
  5. the FAT;
  6. the DIFAT.

  The FAT and DIFAT counts are solved as a fixed point. Every region is zero-padded to a whole sector, and a v4 header occupies a full 4096-byte sector.
- **Directory entries**:
  - Entry ids are assigned depth-first, with siblings in sorted order.
  - Siblings form a balanced binary search tree split at the median. Nodes at the maximum depth are coloured red and all others black, which is always a valid red-black tree. This is verified by tests for 0–300 siblings.
  - The root is always named `Root Entry`.
  - Empty streams and storages use start sector `ENDOFCHAIN` and `0` respectively.
  - Unused slots are zero, with their sibling and child links set to `NOSTREAM`.

### 4.3 API

```cpp
namespace aaf::cfb {
class Container {                       // read-only, validated view
    static auto open(std::unique_ptr<ByteSource>) -> Result<Container>;
    static auto openFile(const std::filesystem::path&) -> Result<Container>;
    auto header() const -> const HeaderInfo&;           // version, sector size, counts, header CLSID
    auto root() const -> const DirEntry&;
    auto entry(EntryId) const -> const DirEntry&;
    auto find(EntryId parent, std::u16string_view) const -> std::optional<EntryId>;
    auto findPath(std::string_view) const -> std::optional<EntryId>;
    auto openStream(EntryId) const -> Result<StreamReader>;
};
class Builder {                         // tree to write
    static auto fromContainer(const Container&) -> Result<Builder>;
    auto addStorage(NodeId parent, std::u16string name, Clsid = {}) -> Result<NodeId>;
    auto addStream(NodeId parent, std::u16string name, StreamData) -> Result<NodeId>;
    void setVersion(Version); void setHeaderClsid(const Clsid&);
};
auto write(const Builder&, ByteSink&) -> Result<void>;
auto writeFile(const Builder&, const std::filesystem::path&) -> Result<void>;   // atomic
}
```

## 5. Layer 2: AAF stored format (`libaafcore`)

This maps CFB to AAF objects. Status: **reading implemented (M2)** in `libs/core/` (namespace `aaf`, headers `aaf/core/{auid,metamodel,value,document}.hpp`); writing is M3. The normative source is the **AAF Stored Format Specification v1.0.1**. The layouts below were checked against it. Real files are the final authority: fixture tests decide, and any discrepancy is recorded here. **This is a clean-room implementation: no AAF SDK source code is used or consulted.**

### 5.1 Objects

- Each persistent object is a CFB **storage** whose CLSID is the object's class AUID.
- Each object storage holds a stream named `properties`:
  - Header: `u8 byteOrder` (`0x4C` 'L' little-endian, `0x42` 'B' big-endian), `u8 formatVersion`, `u16 entryCount`.
  - Index: `entryCount` × `{u16 pid, u16 storedForm, u16 length}`.
  - Values: concatenated in index order.
- Both byte orders must be readable. The byte order applies to the header, index and values. The writer always writes little-endian.
- Values are contiguous (there is no offset field). Index entries with an unknown stored form **must be skipped** using `length`, and are preserved verbatim.
- **File signature**: `{42464141-000d-4d4f-060e-2b34010101ff}` (512-byte sectors) or `{0d010201-0200-0000-060e-2b3403020101}` (4096-byte sectors). **Discrepancy with the stored-format spec**, which says the signature is the root storage's CLSID. In every reference file, and in pyaaf2, the signature is in the **CFB header CLSID** (offset 8), and the root storage's CLSID is the Root class AUID `{b3b398a5-1c90-11d4-8053-080036210804}`. We follow the files. The writer sets the signature that matches the sector size it writes.
- The root storage holds the root object, which has PID `0x0001` for the MetaDictionary and PID `0x0002` for the Header.
- `InterchangeObject::ObjClass` (PID `0x0101`) is **never stored**: an object's class is its storage CLSID. Validation treats it as always present.

### 5.2 Stored forms

| Value | Form | Encoding of value bytes |
|---|---|---|
| 0x82 | Data | Raw typed value (§5.4) |
| 0x42 | Data stream | `u8 byteOrder` + name of a sibling stream holding the bytes (essence, timecode) |
| 0x22 | Strong ref | UTF-16LE NUL-terminated name of the child storage |
| 0x32 | Strong ref vector | Name of `<name> index` stream: header `{u32 count, u32 firstFreeKey, u32 lastFreeKey}` + `count × u32 localKey`. Elements are storages `<name>{<localKey hex>}` |
| 0x3A | Strong ref set | Name of `<name> index` stream: header `{u32 count, u32 firstFreeKey, u32 lastFreeKey, u16 keyPid, u8 keySize}` + `count × {u32 localKey, u32 refCount, key[keySize]}`, sorted by key. `refCount = 0xFFFFFFFF` means sticky |
| 0x02 | Weak ref | `{u16 tag, u16 keyPid, u8 keySize, key bytes}`. `tag` indexes the root `referenced properties` stream |
| 0x12 / 0x1A | Weak ref vector / set | Name of `<name> index` stream: header `{u32 count, u16 tag, u16 keyPid, u8 keySize}` + `count × key[keySize]` |
| 0x03, 0x86, 0x40 | Stored object id / unique object id / opaque stream | Defined but not used by known writers. Preserved verbatim; never written for new data |

- `referenced properties` (root stream): header `{u8 byteOrder, u16 pathCount, u32 pidCount}`, followed by `pathCount` PID lists, each terminated by `0x0000` (`pidCount` includes the terminators). Tag *n* is the *n*-th path, and each path runs from the root object to the strong-ref set holding the targets (for example Header → Dictionary → DataDefinitions).
- Stored form values are bit fields (§1.4 of the stored-format spec). Decode by value, but keep the raw `u16` so unknown forms round-trip.
- Names inside property values (child storage, index and stream names) are UTF-16 in the object's byte order, NUL-terminated. The index stream of a collection named *n* is `n index`. Its element storages are named `n{k}`, where `k` is the local key in lowercase hex with no padding. All of this is confirmed against the reference files.
- **Loader** (`Document::open` / `Document::load`):
  - Objects are loaded breadth-first from the root, with no recursion, so nesting depth is unbounded.
  - A storage claimed by two properties, a missing child storage, a missing index stream, a malformed index or a truncated properties stream are **fatal** errors. The error names the object's storage.
  - A missing data stream is a load **warning**.
  - Limits: a `properties` stream may be at most 64 MiB, and an index stream at most 256 MiB. Counts are checked against stream sizes before allocation. Weak-reference collection indexes with `keySize == 0` are rejected; otherwise any count would pass the size check. This was found by fuzzing.
  - Storage children not referenced by any property (for example extra vendor streams) are recorded in `Object::extraEntries`, so they can be preserved on save.
- **Weak references** are resolved through an index built at load time: for every tag, its path is walked from the root to the target strong-ref set, and each element is keyed by the raw key bytes in that set's index. `Document::resolveWeak(tag, key)` is const and thread-safe.
- Storage names are not semantically significant on read, because they are always resolved via the parent's property value. On write, **preserve by default** the original storage names, sibling order, `formatVersion` and local keys of objects loaded from the source file, to minimise structural diffs against it. This is controlled by `WriteOptions::preserveLayout` (default `true`). For new objects, or when `preserveLayout=false`, generate `<PropertyName>-<pid hex>` (with a `{<key hex>}` suffix for elements), truncated or hashed to stay within the 31-UTF-16-unit CFB name limit. Names must be unique among siblings.

### 5.3 Metamodel

- A **built-in baseline** is compiled in from `model/`: every class, property and type in AAF Object Specification v1.1, with AUIDs and PIDs. That is 116 classes (including the meta classes and `Root`) and 164 types.
  - The Object Specification defines names, the hierarchy, types and required/optional, but **not** the AUIDs or PIDs.
  - The machine-readable IDs come from the pyaaf2 1.7.1 model tables (`aaf2/model/classdefs.py`, `typedefs.py`, plus `Root` from `metadict.py`; MIT, attribution in `model/NOTICE`). The Avid extensions in `aaf2/model/ext` are **not** included, because files define their own extensions.
  - `tools/gen_model.py json` writes `model/baseline.json`, and `tools/gen_model.py cpp` writes `libs/core/src/generated/baseline_model.cpp`, which contains `constexpr` tables described in `baseline_tables.hpp`. Both outputs are committed.
  - CI checks that the C++ matches the JSON (`gen_model.py check`), and pytest checks that the JSON matches the pinned pyaaf2.
  - Properties that pyaaf2 lists without a fixed PID (newer MXF-derived descriptors, and `TypeDefinitionGenericCharacter::CharacterSize`) have PID `0` in the baseline, meaning "dynamic": the file's MetaDictionary assigns the PID.
  - Cross-check, run as a unit test: every property definition found in a reference file's MetaDictionary has the same PID as the baseline, wherever the baseline PID is fixed. All 58 reference files pass. The check against the Object Specification's names is manual, because the PDF is not in CI.
  - Reference files carry only a partial MetaDictionary (the SDK samples hold 45–79 classes each), so the files alone are not a sufficient source.
- On open, the file's MetaDictionary is parsed and **merged** with the baseline. File-defined extension classes, properties and types (dynamic PIDs ≥ 0x8000) are fully supported. The editor is data-driven, so unknown classes are still shown and editable through their definitions.
  - **Bootstrapping**: the objects are first loaded structurally (stored forms only; no types are needed). The MetaDictionary objects are then interpreted using the **baseline** meta-class property PIDs, which are fixed.
  - **Class definitions**: `Identification`, `Name`, `ParentClass` (a parent equal to itself means none), `IsConcrete`, and the `Properties` set.
  - **Property definitions**: `Identification`, `Name`, `Type`, `IsOptional`, `LocalIdentification` and `IsUniqueIdentifier`. `Type` may be stored either as AUID data or as a weak reference; both are accepted.
  - **Type definitions** are interpreted per `TypeDefinition*` class.
  - **Merge rule**: file definitions replace baseline fields where both exist. Extendible enumerations take the union of their elements. A file PID that differs from a fixed baseline PID is a load warning. Every definition records its source (`baseline`, `file` or `both`).
- Type categories to support: Integer (1/2/4/8, signed and unsigned), Character, String, Enum, ExtEnum, Record, FixedArray, VarArray, Set, Rename, StrongObjRef, WeakObjRef, Stream, Indirect, Opaque.
- If a property cannot be decoded (unknown PID and no definition), it is kept as **opaque bytes with its stored form** and written back unchanged.

### 5.4 Values

`aaf::Value` is a variant over:

- null, bool, `int64`, `uint64`;
- UTF-8 string;
- `Auid`, `MobId`;
- `Enum{value, name}`, `ExtEnum{value, name}`;
- `Record{names, values}` and `Array`;
- `Indirect{type, value}`;
- `Opaque{type, bytes}`;
- `Bytes`.

It covers data properties only. Object references and streams are represented by the stored property payloads (§5.5).

Decoding (`decodeValue(model, type, bytes, bigEndian)`):

- Renames are resolved first.
- `Boolean` becomes bool. Other enumerations decode their element integer and look up its name.
- The `AUID` and `MobIDType` records become `Auid` and `MobId`. Other records decode field by field.
- Arrays and sets require fixed-size elements. Arrays of `Character` decode as NUL-separated string lists.
- Strings are UTF-16 up to the first NUL. Strings of 1-byte generic characters are treated as Latin-1.
- `Indirect` and `Opaque` start with `u8 byteOrder` and a 16-byte type AUID.
- Every size mismatch, unknown type, or nesting deeper than 32 is an error, never a crash.
- Encoding (the inverse) arrives with M3/M4, together with codec round-trip tests.

`MobId::toString()` produces `urn:smpte:umid:…` using the same algorithm as pyaaf2, including its special case for half-swapped material numbers.

### 5.5 Object graph

- `Document` owns the source `cfb::Container` and every `Object`. Each object has:
  - a session-stable `ObjectId` (u64, never reused within a session; object 0 is the root);
  - its class AUID;
  - its parent and the owning PID;
  - its storage entry and name, byte order and `formatVersion`;
  - its properties in file order, and `extraEntries`.
- Each `Property` is `{pid, storedForm, payload}`. The payload is one of:
  - `DataProperty`: raw bytes, decoded on demand by `Document::decode`;
  - `StrongRefProperty`;
  - `StrongRefVectorProperty` (with its local keys and free-key range);
  - `StrongRefSetProperty` (with the index entries: local key, reference count and key bytes; and the key PID and size);
  - `WeakRefProperty`;
  - `WeakRefCollectionProperty`;
  - `StreamProperty` (byte order, name, entry and size);
  - `UnknownProperty` (raw bytes).

  Everything the file stores is kept, so that the M3 writer can reproduce it.
- Weak refs keep their raw `{tag, keyPid, key}` and are resolved through the load-time index. Dangling refs are flagged by validation.
- On save, weak-ref keys are re-derived from each target's current unique identifier, and `referenced properties` is regenerated.
- Stream properties hold a `StreamHandle`, which is either a reference into the source file (lazy) or new in-memory or file-backed data.
- Loading is eager for objects and properties and lazy for streams. Target: open a 50k-object file in under 2 s.

### 5.6 Validation (`validate()`)

Returns the load diagnostics plus a list of diagnostics `{severity (info, warning or error), objectId, pid, message}`. Implemented in M2:

- **Warning:** an unknown class (the object is then skipped), an undefined PID, a property its class does not define, or an unknown stored form.
- **Error:** a missing required property (except the implicit `ObjClass`), or a data value that fails to decode.
- **Stored form must match the property type:** strong ref, strong vector or set, weak ref, weak vector or set, data stream, otherwise data. A mismatch is an error, except that swapping vector and set is **info**: it's a known SDK/Avid quirk, and `OperationGroup.Parameters` is stored as a set although it is typed as a vector.
- **Weak references must resolve**, or else name a definition known to the merged model. SDK files omit baseline definitions from their MetaDictionary but still reference them. A reference that does neither is an error.
- The strong-ref structure is a tree by construction: the loader rejects a storage claimed twice.

All 58 reference files report 0 errors and 0 warnings; the only notes are the `Parameters` quirk.

Later milestones: MobIDs unique; definitions referenced by components exist in the Dictionary; timeline sanity (segment lengths versus slot lengths, transitions flanked by segments, non-negative lengths).

## 6. Layer 3: Timeline projection (`libaaftl`)

A read/write view derived from the object graph. It is recomputed incrementally after each command and **never stored separately**.

- **Mobs**: CompositionMob, MasterMob and SourceMob (with File/Tape/Film/Import/Recording descriptors).
- **Slots**: TimelineMobSlot (edit rate, origin), EventMobSlot (markers), StaticMobSlot. Each has a track kind derived from the data definition (picture, sound, timecode, edgecode, descriptive metadata, other).
- **Segments** are flattened into `TimelineItem{objectId, kind, start, length, edit rate, children}`, where kind is SourceClip, Filler, Transition, OperationGroup (effect, with inputs), EssenceGroup, Selector, NestedScope/ScopeReference, Timecode or DescriptiveMarker.
- **Source resolution**: follows SourceClip → MasterMob → SourceMob chains to the physical essence (descriptor plus locators or embedded EssenceData). Cycles and missing mobs are reported, not fatal.
- Time is exact rational arithmetic (`aaf::Rational`, 64-bit numerator and denominator with overflow checks). Display conversions (timecode, seconds, samples) happen only at the UI boundary.
- Audio gain, pan and keyframes (OperationGroup parameters, VaryingValue/ControlPoints) are exposed read-only in v1.

### 6.1 Editorial operations

Each operation is a command (§7) that edits the underlying objects:

| Op | Semantics |
|---|---|
| move | Move an item within or between compatible tracks. The vacated space becomes Filler (lift) or closes (ripple, optional). |
| trim | Adjust in or out; for SourceClips this also adjusts StartTime. Clamp to source length where known. |
| split | Split a segment at a time, duplicating the object with adjusted StartTime and Length. |
| delete | Lift (replace with Filler) or ripple. |
| insert / overwrite | Place a SourceClip referencing an existing mob/slot. |
| add/remove track | Create or remove a TimelineMobSlot with a Sequence. |
| rename | Name of mobs, slots and components. |
| relink | Edit NetworkLocator URLs, or batch find/replace on locator paths. |
| markers | Add, edit or remove DescriptiveMarkers and comments. |

Adjacent Fillers are merged after every op. Transitions adjacent to an edited point are kept valid or removed, with a warning.

## 7. Layer 4: Edit session (`libaafedit`)

- `Session` = `Document` + undo stack + dirty flag + change notifications.
- A `Command` is an object with `apply(Document&) → expected<ChangeSet>` and `revert`. Low-level primitives are setProperty, removeProperty, createObject, deleteObjectTree, insertIntoCollection, removeFromCollection and moveInCollection. Higher-level ops (inspector edits, §6.1) are composed from them as `CompositeCommand`s.
- A `ChangeSet` lists the changed objects and properties. The UI uses it to refresh only affected nodes and tracks.
- Every command is validated before commit. Commands that would violate type or ownership rules are rejected with an error, and nothing is applied.
- Unlimited undo/redo in memory. Save does not clear history.
- Deleting an object that is a weak-ref target requires an explicit "delete anyway" flag. The dangling refs are then reported.

## 8. Applications

### 8.1 `aaftool` (CLI)

```
aaftool dump <file> [--json] [--depth N] [--header|--metadict]
                                              object tree from the root, Header or MetaDictionary (M2)
aaftool cfb <file>                            raw CFB directory listing (header info + tree)
aaftool cfb-roundtrip <in> <out> [--v3|--v4]  rewrite the CFB container only (no AAF parsing)
aaftool validate <file> [--json]              diagnostics; exit 1 on errors (M2)
aaftool roundtrip <in> <out>                  read + write, no edits
aaftool timeline <file> [--mob NAME|ID]       text timeline
aaftool extract <file> <mobid> <out>          dump embedded essence stream
```

The JSON output schema is the same one the RPC bridge uses (§8.3). **Canonical dump form** (`dump --json`):

- **Object:** `{"class": <class name, or AUID if unknown>, "properties": {<property name, or "0x%04x" if undefined>: <value>}}`, in file order.
- **Strong references:** a strong ref is a nested object; a strong vector or set is an array of objects.
- **Weak references:** a weak ref is its key as a string; a weak vector or set is an array of key strings. Keys of 16 bytes format as an AUID, 32 bytes as a MobID URN, anything else as hex.
- **Streams:** `{"stream": name, "size": n}`.
- **Scalars:** booleans, integers and strings map to JSON scalars.
- **Identifiers and enumerations:** an AUID is `"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"`, and a MobID is its URN. Enumerations and extendible enumerations give the element name, falling back to the number or AUID.
- **Composites:** records map to objects, arrays to arrays, and an indirect value to its inner value.
- **Opaque and undecodable data:** opaque values are `{"opaque": type, "bytes": hex}`; undecodable data is `{"error": message, "bytes": hex}`; unknown stored forms are `{"storedForm": n, "bytes": hex}`.
- The text dump shows the same tree, indented, with set elements labelled by their key.

### 8.2 Editor host (`apps/editor`)

- Uses the [webview/webview](https://github.com/webview/webview) library (WebKitGTK on Linux, WKWebView on macOS, WebView2 on Windows).
- The UI assets are built by Vite and embedded into the binary at build time (a CMake step generates a resource table). No network access; the content security policy allows only the embedded origin.
- The native file open/save dialogs and the menu are provided by the host, with thin per-OS code behind one interface.
- All model work runs on a worker thread. The UI thread never blocks on I/O.

### 8.3 RPC bridge

- JSON-RPC 2.0 over `webview_bind` (UI → host) and `webview_eval` dispatching events (host → UI).
- Methods (initial set):
  - `doc.open(path)`, `doc.save()`, `doc.saveAs(path)`, `doc.close()`, `doc.validate()`
  - `tree.children(objectId, offset, limit)` (paged; large collections must not flood the bridge)
  - `object.get(objectId)` returns class, properties with type info, and decoded values
  - `object.setProperty(objectId, pid, value)`, `object.removeProperty`, `object.create(parentId, pid, classAuid, init)`, `object.delete(objectId, force)`
  - `timeline.mobs()`, `timeline.get(mobId)`, `timeline.op(op)` (§6.1 ops)
  - `edit.undo()`, `edit.redo()`, `edit.history()`
  - `search.query({text?, class?, property?})`
- Events: `doc.changed{changeSet}`, `doc.dirty{bool}`, `diag.updated`, `progress{task, fraction}`.
- Values are encoded in JSON as tagged objects, e.g. `{"t":"Rational","n":25,"d":1}` or `{"t":"MobID","v":"060a2b34…"}`. 64-bit integers are strings. The schema lives in `ui/src/rpc/schema.ts` and `libs/…/json_schema.hpp` and is checked by a round-trip test.

### 8.4 UI (`ui/`)

TypeScript, Vite and Svelte 5. The timeline is drawn on `<canvas>`.

- **Layout**: left, object tree (virtualised); centre, a tabbed timeline per CompositionMob; right, a property panel; bottom, diagnostics and the undo history.
- **Property panel**: a type-aware editor per value kind (numeric with range checks, enum dropdown, rational, timestamp, AUID/MobID with copy and paste, string, record fields, arrays with add/remove/reorder). Read-only and required markers come from the metamodel. There is a raw hex view for opaque values.
- **Timeline**: tracks with headers (kind, name, edit rate); zoom and scroll; a timecode ruler (from the timecode track if present, else from 0); clip selection syncs with the tree and property panel; drag to move and trim; keyboard shortcuts for split, delete (lift) and shift+delete (ripple); markers lane; clips whose source is missing or offline are highlighted.
- **Cross-view sync**: selecting a clip reveals its object in the tree and vice versa.
- Light and dark themes. Every action is reachable from the keyboard.

## 9. Fidelity requirements (acceptance criteria)

1. **Lossless round-trip**: for every fixture, `open → save` without edits yields a file that re-opens to a semantically identical graph: the same classes, property values, collection order, weak-ref targets and stream bytes. Byte-identical output is *not* required.
2. Unknown classes, properties and opaque data survive round-trip.
3. Files written by us open in: pyaaf2 (automated in CI), the AAF SDK InfoDumper (automated if feasible), and Avid Media Composer, Pro Tools and DaVinci Resolve (manual release checklist).
4. Editing only touches the objects in the command's ChangeSet. Diffing the graph before and after an edit shows no other changes.
5. Malformed input never crashes: every fuzz target runs with no findings in the nightly job (10 minutes per target, under ASan/UBSan).

## 10. Testing

- **Unit (Catch2)**: CFB structures (header, FAT chains, mini stream, directory tree ordering), every stored form, every type codec, metamodel merge, validation rules, rational maths, each timeline op (before and after graph assertions), and undo/redo symmetry (apply, revert, compare).
- **Generated fixtures**: `tools/gen_fixtures.py` (pyaaf2) generates small deterministic files covering every feature, including extensions and big-endian where possible. These are committed in `tests/fixtures/generated/`.
- **Reference fixtures**: the AAF files published in these upstream repos form the main real-world corpus:

  | Source | Location | Licence | Notes |
  |---|---|---|---|
  | AAF SDK 1.2.0 (AMWA) | **Committed** in `tests/fixtures/aafsdk/`: the 6 `*.aaf` files from `AAF-src-1.2.0.zip` (SourceForge), unmodified. All use 512-byte sectors | AAF SDK Public Source License v2.0 (`LEGAL/AAFSDKPSL.TXT` kept alongside, as that licence requires) | Canonical SDK output. `PROVENANCE.md` records the archive URL, the archive SHA-256 and a per-file SHA-256. No SDK source code is kept. |
  | pyaaf2 | `github.com/markreidvfx/pyaaf2` `tests/test_files/**` (includes `sector_size_512.aaf` and `retimes/`) | MIT | Includes a v3 (512-byte sector) file |
  | OpenTimelineIO AAF adapter | `github.com/OpenTimelineIO/otio-aaf-adapter` `tests/sample_data/*.aaf` | Apache-2.0 | Real Avid/Premiere/Resolve exports: effects, transitions, nesting, multicam, markers |

  - The pyaaf2 and OTIO files are **not committed**. `tools/fetch_fixtures.py` (`fetch` is the default; `pin` re-resolves `main` to a commit and rewrites the manifest) downloads them into `tests/fixtures/external/<source>/`, driven by `tests/fixtures/external/manifest.json`. The manifest records the repo URL, pinned commit, path and SHA-256 of each file, and the script verifies every hash.
  - The fetch is idempotent and cached, and CI caches the directory keyed on the manifest hash.
  - The vendored AAF SDK files are always present, so their tests run on every PR. Tests that need fetched fixtures are tagged `[external]` and skipped with a message when the files are absent.
  - Every reference file must pass: open without error, `validate` with no errors (known upstream defects are listed in the manifest as expected diagnostics), lossless round-trip (§9.1), and the pyaaf2 cross-check.
  - For the OTIO files, a test compares our timeline projection (track count, clip count, record in/out, source in/out per clip) with the expected values in `tests/fixtures/external/otio_expectations.json`. Those values are generated once by running the OTIO AAF adapter (`tools/gen_otio_expectations.py`) and committed.
- Additional real-world samples (anonymised, redistributable) can be added to the manifest with provenance notes. Large samples are always fetched, never committed.
- **Cross-check**:
  - Container level (M1): `tools/crosscheck_cfb.py <aaftool>` rewrites every reference file with `aaftool cfb-roundtrip` as v3 and v4. pyaaf2 must then read an identical directory tree, CLSIDs and stream bytes, and the same mob IDs.
  - Object level (M2): `tools/crosscheck.py <aaftool>` compares `aaftool dump --json --header` with pyaaf2's reading of the Header tree, reduced to the same canonical form. The comparison covers every object, property, decoded value, weak key and stream.
    - Strong-set order is ignored.
    - Python sets map to sorted lists; `datetime` values map back to the `TimeStamp` record, with microseconds as `fraction`.
    - One pyaaf2 quirk is accepted: it normalises a stored 0/0 rational to 0/1.
    - All 58 reference files match.
- Python tool versions are pinned in `tools/requirements.txt` (pyaaf2 1.7.1, pytest).
- **Corruption**: unit tests cover a bad header, truncation, FAT cycles, directory cycles and 900 deterministic random mutations. `tests/fuzz/fuzz_cfb.cpp` (libFuzzer: open, read every stream, rewrite) and `tests/fuzz/fuzz_document.cpp` (load an AAF document, decode every data property, validate) each run 10 minutes nightly, seeded from the SDK fixtures. The M2 baseline was 16.7k document executions in 5 minutes with no findings; each input is a whole AAF file. The M1 baseline was 1.48 M executions in 5 minutes with no findings.
- **UI**: Vitest for the frontend logic and the RPC codec. Playwright smoke test against a headless build of the UI with a mock bridge.

## 11. CI/CD (GitHub Actions, frugal)

- **`ci.yml`** (every PR and push to main):
  - lint: clang-format 22 check and pytest for `tools/`;
  - Linux GCC 14 Debug and Clang 22 Release, with `-Werror`, unit tests, and clang-tidy (`.clang-tidy`, warnings as errors) on the Clang job.
  - LLVM 22 comes from apt.llvm.org (`.github/actions/setup-llvm`).
- **`full.yml`** (push to main, nightly at 03:17 UTC, manual, or PRs that change it):
  - Windows MSVC and macOS AppleClang builds and tests;
  - ASan/UBSan tests with the external fixtures (cached by manifest hash) and the pyaaf2 cross-check;
  - TSan tests (including concurrent stream reads through one `FileSource`);
  - 10-minute fuzzing.
- UI lint and tests are added with M5.
- **Tags `v*`**: a release workflow builds on a three-OS matrix and publishes a GitHub Release with these assets:

  | OS | Runner | Artifacts |
  |---|---|---|
  | Linux x86_64 | `ubuntu-latest` (built on the oldest supported Ubuntu LTS image, for glibc compatibility) | `aaf-editor-<ver>-linux-x86_64.AppImage`, `aaf-editor-<ver>-linux-x86_64.tar.gz` (editor + `aaftool`) |
  | macOS arm64 + x86_64 | `macos-latest` | `aaf-editor-<ver>-macos-universal.dmg` (`.app` bundle), `aaftool-<ver>-macos-universal.tar.gz` |
  | Windows x86_64 | `windows-latest` (MSVC) | `aaf-editor-<ver>-windows-x86_64.zip` (editor + `aaftool`, portable) |

  - **No code signing and no notarization.** The release notes explain how to open unsigned apps (macOS: right-click → Open, or `xattr -dr com.apple.quarantine`; Windows: SmartScreen "More info → Run anyway").
  - Windows uses the WebView2 runtime that ships with Windows 10 and 11. The loader is statically linked, and the runtime is not bundled.
  - Linux requires WebKitGTK at runtime (documented). The AppImage bundles it where feasible.
  - A `SHA256SUMS.txt` is attached to every release. The version comes from the tag and is embedded in the binaries.
  - Releases are the only jobs that build packaging artifacts. PR CI builds no packages.

## 12. Milestones

| # | Deliverable | Exit criterion |
|---|---|---|
| M1 ✅ | `libaafcfb` read + write, `aaftool cfb` / `cfb-roundtrip`, fixture fetch script + manifest, fuzz target, CI | All 58 reference files round-trip at the CFB level (v3 and v4) and pass the pyaaf2 cross-check; fuzz target running |
| M2 ✅ | Stored-format reader, baseline metamodel, `aaftool dump/validate` | All 58 reference files load and validate with 0 errors, and their Header trees match pyaaf2 exactly |
| M3 | Stored-format writer, `aaftool roundtrip` | §9.1–9.3 (automated parts) pass |
| M4 | Edit session + primitive commands | Undo/redo symmetry tests pass |
| M5 | Webview host, RPC, tree and property inspector | Edit and save any property from the UI |
| M6 | Timeline projection + read-only timeline view | Fixtures render correctly, with selection sync |
| M7 | Timeline editing ops (§6.1) | Op tests pass; edited files open in Resolve and Pro Tools |
| M8 | Packaging, release pipeline, docs | A tagged release publishes unsigned binaries for Linux, macOS and Windows (§11) |
| Later | Create-new-file templates, Edit Protocol conformance checks, OTIO import/export, keyframe editing, essence waveform/thumbnail previews | — |

## 13. Decisions log

| Date | Decision |
|---|---|
| 2026-09-27 | JSON: nlohmann/json, in `apps/` only (libraries stay dependency-free) |
| 2026-09-27 | Frontend: Svelte 5 + TypeScript + Vite |
| 2026-09-27 | Writer preserves the source storage layout by default (§5.2) |
| 2026-09-27 | Licence: MIT |
| 2026-09-27 | Reference fixtures come from the AAF SDK, pyaaf2 and otio-aaf-adapter, fetched and pinned by hash (§10) |
| 2026-09-27 | Releases for Linux, macOS and Windows from CI, unsigned and not notarized (§11) |
| 2026-09-27 | Clean-room library built from the AAF specifications. The only thing taken from the AAF SDK is its 6 sample `.aaf` files, committed as fixtures |
| 2026-09-27 | Baseline metamodel IDs come from the pyaaf2 model tables (MIT), cross-checked against the Object Spec and reference files |
| 2026-09-27 | Spec PDFs kept local only (not redistributable), fetched by script |
| 2026-09-27 | AAF file signature lives in the CFB header CLSID, not the root storage CLSID (the files contradict the stored-format spec) |
| 2026-09-27 | CI: frugal PR pipeline on Linux only; Windows, macOS, sanitizers, external fixtures and fuzzing run nightly and on main |
| 2026-09-27 | Baseline excludes pyaaf2's Avid extension tables; dynamic-PID properties have PID 0 in the baseline |
| 2026-09-27 | `ObjClass` is implicit; strong vector and set swaps are info-level quirks; weak refs to baseline definitions missing from the file are valid |
| 2026-09-27 | Load errors are fatal for structural damage (missing storages or indexes, malformed streams), and warnings for missing data streams |

**Licensing note:** AAF SDK material is used only as test data. No SDK code is used or consulted.

## 14. Open questions

None currently.

## References

- [MS-CFB] Microsoft Compound File Binary File Format specification.
- AAF specification PDFs, shipped in the AAF SDK 1.2.0 archive (`AAF/doc/`). They are © 2004 AAF Association and not redistributable, so they are not committed. `tools/fetch_specs.py` downloads the archive, verifies its SHA-256 and extracts them to `docs/reference/`:
  - `aafobjectspec-v1.1.pdf`: AAF Object Specification v1.1 (normative for the object model)
  - `aafstoredformatspec-v1.0.1.pdf`: AAF Stored Format Specification v1.0.1 (normative for §5)
  - `aafcontainerspec-v1.0.1.pdf`: AAF Low-Level Container Specification v1.0.1 (Structured Storage v3; read alongside [MS-CFB])
  - `aafeditprotocol.pdf`: AAF Edit Protocol
  - `aafobjectspec-v1.0.1.pdf`: previous object spec, for older files
- pyaaf2 (test-time cross-check and fixture generation): https://github.com/markreidvfx/pyaaf2
- OTIO AAF adapter (reference files, timeline expectations): https://github.com/OpenTimelineIO/otio-aaf-adapter
