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
    aaftool/    CLI: cfb, cfb-roundtrip, dump, validate, roundtrip, extract, set-essence
    rpc/        aaf::rpc JSON-RPC server over an edit session (nlohmann/json)
    editor/     aafedit: webview host (C API wrapper, worker thread, native dialogs)
  ui/           Svelte 5 + TypeScript frontend (Vite), built to one HTML file embedded in aafedit
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
  - rename over the target (`std::filesystem::rename` on POSIX; on Windows a POSIX-semantics rename, falling back to `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`);
  - on failure, delete the temporary file and leave the target untouched.
  - **Saving over the source file** while the document still reads streams from it is supported:
    - On POSIX, the rename leaves the old inode readable through the open handle.
    - On Windows, `FileSource` opens files with `CreateFileW(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)`. The replacement uses `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)` (Windows 10 1607+, NTFS), falling back to `MoveFileExW`. A plain `MoveFileExW` over a file that is still open fails, as CI showed.
    - A test saves a document over its own source twice (M3).
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
- Both byte orders must be readable. The byte order applies to the header, index and values. The writer keeps each loaded object's byte order, so its raw data bytes stay valid. The `referenced properties` stream keeps its byte order too. New objects are created little-endian, with `formatVersion` `0x20` (the only value in the reference files). Edited values are encoded in their object's byte order.
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
- Storage names are not semantically significant on read, because they are always resolved via the parent's property value.
- **Writer** (M3, `aaf/core/writer.hpp`: `buildContainer`, `write`, `save`, with `WriteOptions{preserveLayout = true, version}`):
  - **Output**: the writer turns the `Document` into a `cfb::Builder`. It writes:
    - each object's `properties` stream (header, index, then values, in the object's stored property order);
    - child storages;
    - index streams (vector: count, free keys and local keys; set: also the key PID, key size and entries with reference counts and keys; weak collections: count, tag, key PID, key size and keys);
    - data streams, copied lazily from the source container;
    - `extraEntries` subtrees, copied verbatim;
    - the regenerated `referenced properties` stream.
  - **Preserved per entry**: storage CLSIDs are the object class AUIDs. Entry state bits and timestamps are copied from the matching source entries.
  - **Header signature**: the header CLSID is reset to the AAF signature that matches the output sector size. Any other header CLSID is kept.
  - **`preserveLayout = true`** (the default) keeps storage and index names, local keys, free-key ranges, `formatVersion` and byte orders. The container tree of an unmodified document is then **identical** to its source, including every stream's bytes and entry metadata. This is verified for all 58 reference files at both sector sizes.
  - **`preserveLayout = false`** changes names and keys:
    - names are generated as `<PropertyName>-<pid hex>` (`generatedStorageName`);
    - characters outside printable ASCII, and `/ \ : ! { }`, become `_`;
    - the base is truncated so that the longest element name, with its `{ffffffff}` suffix, fits in 31 units; the PID suffix keeps names unique;
    - local keys are renumbered from 0, with `firstFreeKey = count` and `lastFreeKey = 0xFFFFFFFF`;
    - set keys and reference counts are kept.
  - A property value longer than 65535 bytes is an error.

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
  - **Merge rule**: file definitions replace baseline fields where both exist, with one exception: the **baseline property type wins**. A conflicting file type is recorded as a load note. This matters because AAF SDK files declare `MemberNames` and `ElementNames` as `aafString` but store NUL-separated string arrays, which is what the baseline type `aafStringArray` describes. Extendible enumerations take the union of their elements. A file PID that differs from a fixed baseline PID is a load warning. Every definition records its source (`baseline`, `file` or `both`).
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
- **Encoding** (`encodeValue`, M4) is the inverse:
  - Integers accept either signedness, with range checks.
  - Enumerations accept `Enum` (the name wins if set), a number, or a name string.
  - Extendible enumerations accept an AUID, an `ExtEnum`, or a name.
  - Records need every field, matched by name.
  - Strings are encoded as UTF-16 followed by a NUL.
  - `Bytes` is accepted as a raw value for any type.
  - **Test**: for every decoded data property in the 58 reference files (235k values), `encode(decode(b)) == b`. The only exception is strings stored with extra trailing NUL padding, which must still round-trip at the value level. Unedited properties keep their raw bytes, so the padding is preserved anyway.
  - `Value` and all stored payload types have `operator==`.

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
- On save, weak references are written from their stored `{tag, keyPid, key}`, and `referenced properties` is regenerated from its current paths. Edits keep keys consistent (§7): changing a set element's unique identifier rewrites the set index entry and every weak reference to it.
- Stream properties reference their source entry, which is copied lazily on save, unless an edit has attached new contents. `StreamProperty::data` is a `std::shared_ptr<const cfb::ByteSource>`: either a `MemorySource`, or a `FileSource` for large essence, which is then read in chunks only when saving and never loaded into memory.
  - `readStream(doc, stream, offset, out)` and `copyStream(doc, stream, sink)` read either kind, so extraction, previews and the RPC bridge don't need to care where the bytes live.
  - In the CFB builder, `StreamData` has a third alternative, `SharedSource`. `sizeOf` and `readStreamData` work for all alternatives, and a container stream is opened once per copy, so its sector chain is not walked for every chunk.
- **Editing hooks** (M4): `Document::mutableObject`, `addObject`, `mutableReferencedProperties`, `rebuildIndexes`, `isAttached` and `tagTarget`. They are low-level: only `aaf::edit` uses them, and it maintains the invariants. Detached objects stay in the object table so that ids remain stable for undo. They are not validated and not written.
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

Status: **projection implemented (M6)** in `libs/timeline/` (namespace `aaf::timeline`, headers `aaf/timeline/{rational,timeline}.hpp`); editing is M7. It is a read-only view derived from the object graph, **never stored separately**. `Projector` builds a MobID index (mobs and EssenceData) on construction, so a new one is constructed after each edit; that takes milliseconds.

- **Time**: `Rational` is exact 64-bit arithmetic, always in lowest terms with a positive denominator.
  - `add`, `subtract`, `multiply` and `divide` return `Result` and fail on overflow. The checks are portable: no `__int128` or compiler builtins, because MSVC has neither.
  - Comparison is exact without overflow, using a continued-fraction (Euclid) method.
  - `convertPosition(position, fromRate, toRate)` rounds toward negative infinity.
  - Conversions to timecode, seconds or samples happen only in the UI.
- **Mobs**: `mobs()` lists every attached mob as `{object, mobId, name, kind (composition, master, source or other), tracks, topLevel}`.
  - A composition is top-level if its `UsageCode` is `Usage_TopLevel`; without a usage code, if no SourceClip references it.
  - Sort order: top-level first, then by kind, then by name.
- **Tracks** (`project(mob)`): one per slot, in slot order, with:
  - `slotId`, `name`, `physicalNumber`;
  - `kind`: picture, sound, timecode, edgecode, descriptive metadata, data or other. It comes from the segment's DataDefinition weak key, matched against the known SMPTE and legacy AUIDs (listed in `projection.cpp`); an unknown key falls back to the definition's `Name`.
  - `slotKind`: timeline, event or static;
  - `editRate` and `origin`;
  - `length`: the segment's `Length`, else the end of the last item.
  - A **track-level effect** (the slot's segment is an OperationGroup, for example Avid's "Audio Pan" on a whole track) is unwrapped: `effects` lists the group or groups, and `items` are the content of the first input. This matches how editors and OTIO present such tracks.
- **Items**: `{object, kind, className, start, length, hasLength, label, source?, effect, timecode?, nested, comment}`.
  - **Layout**: a Sequence is laid out with a cursor. A segment starts at the cursor and advances it by its length. A **Transition starts at `cursor − length` and moves the cursor back by its length**, so it overlaps the end of the previous segment and the start of the next. Events (markers) use their own `Position`.
  - **Kinds**: sourceClip, filler, transition, operationGroup, essenceGroup, selector, nestedScope, scopeReference, pulldown, sequence, timecode, edgecode, marker (DescriptiveMarker or CommentMarker), event and other.
  - **Nesting**: effect inputs, essence group choices, a selector's selected segment and its alternates, nested scope slots, and pulldown inputs become `nested` item lists, up to 16 levels deep; deeper nesting is reported as a warning.
  - **Source clips** carry `SourceReference{mobId, slotId, startTime, mob?, mobName, mobKind, original}`. `original` is true for a null MobID, which ends a chain.
  - **Labels**: clips show the referenced mob's name, or "Missing source" or "Original source". Effects and transitions show their OperationDefinition name, and markers show their comment.
  - The mob's `timecode` is the first Timecode segment on a timecode track: start, fps and drop-frame flag.
- **Source resolution** (`resolve(sourceClip)`):
  - It follows SourceID and SourceMobSlotID through the mobs. StartTime plus the offset into the current clip is converted into the referenced slot's edit rate, the SourceClip covering that position is found, and the walk repeats. It ends at a null MobID or at a non-clip, and stops after 64 links.
  - Each link is `{mob, mobId, name, kind, slotId, position, editRate, descriptor}`.
  - Status is resolved, missingMob, missingSlot or cycle. Problems are reported, never fatal.
  - `essence` describes the deepest SourceMob with a FileDescriptor: `{embedded (an EssenceData with the same MobID exists), essenceData, locators (NetworkLocator URLs), descriptor class}`.
- **Tests**:
  - rational arithmetic and overflow;
  - contiguous layout for more than 100 tracks without transitions in the SDK sample;
  - transition overlap on `transitions.aaf`;
  - resolution through master to source mobs, with embedded and linked essence, and a deliberately broken reference reported as missingMob;
  - **comparison with the OpenTimelineIO AAF adapter**: `tools/gen_otio_expectations.py` records, with `otio-aaf-adapter` 2.0.0 and OTIO 0.18.1, each OTIO sample's top-level timeline as `[type, start, duration]` per video or audio track, in `tests/fixtures/external/otio_expectations.json` (committed). Our projection must equal it for every compared track (32 at present). Excluded are tracks that are empty (OTIO drops filler-only tracks), files where a track references a nested composition or NestedScope (OTIO flattens those into extra tracks, while we keep them as single items on purpose), and tracks with OTIO transitions (OTIO does not overlap transitions).
- **`aaftool timeline <file> [--mobs] [--mob NAME|ID] [--json]`** lists mobs, or prints a mob's tracks and items (by default the first composition).
- Audio gain, pan and keyframes (OperationGroup parameters, VaryingValue/ControlPoints) can be inspected in the property panel. They have no dedicated timeline display yet.

### 6.1 Editorial operations

Status: **implemented (M7)** in `aaf/timeline/edit.hpp` (namespace `aaf::timeline::ops`).

- **Execution**: every operation runs inside an `edit::Transaction`, so it is validated, atomic and undoable like any other command (§7).
- **Positions** are in the track's edit units, measured from the start of its sequence (as projected).
- **Track editing**:
  - An operation edits the Sequence inside the slot, unwrapping track-level effects.
  - A slot holding a single SourceClip or Filler is first wrapped in a new Sequence, with the same DataDefinition and Length.
  - Slots holding Pulldown, Timecode, EdgeCode or NestedScope are rejected, and so are items that are not directly in a track's sequence.
- **Normalization**: after every operation, adjacent Fillers are merged, zero-length Fillers are removed, and the Sequence's `Length` and those of the wrapping effects are recomputed (sum of segments minus sum of transitions).

| Op | Semantics |
|---|---|
| `split(slot, position)` | Splits the **clip** under `position` (a deep copy becomes the right part, with `StartTime` advanced by the left part's length) and returns the right part. Splitting filler is refused, because it would merge straight back; a position inside a transition is refused. |
| `lift(item)` | Replaces a segment with Filler of the same length. For a Transition, it makes a **cut at the midpoint**: the previous segment is shortened by ⌈L/2⌉, the next by ⌊L/2⌋, and the next clip's StartTime moves by ⌊L/2⌋, so timing is unchanged. |
| `rippleDelete(item)` | Removes a segment and closes the gap, turning the transitions on either side into cuts first. For a transition, it behaves like lift. |
| `trim(item, head \| tail, delta, ripple)` | Moves an edge. A **roll** also resizes the neighbour (and adjusts its StartTime for a tail edge); a **ripple** changes only this segment. Segments must keep length ≥ 1 (a filler neighbour may reach 0 and is removed). StartTime must stay ≥ 0, and clips must stay within their source slot's `Length` when it can be resolved. Edges touching a transition are refused ("remove the transition first"). |
| `place(slot, position, segment, insert)` | Places a detached segment with the same DataDefinition. Insert cuts at `position` and pushes later material. Overwrite cuts at both ends and removes what lies between. A range overlapping a transition is refused. Placing past the end adds Filler first. |
| `placeClip(slot, position, sourceMob, sourceSlot, sourceIn, length, insert)` | Creates a SourceClip (DataDefinition from the track; SourceID, SourceMobSlotID, StartTime, Length) and places it. The source slot must exist. |
| `move(item, toSlot, position, ripple)` | Leaves Filler behind (or closes the gap with `ripple`), then overwrites at the target, which may be another track with the same data kind. Transitions cannot be moved. |
| `addTrack(mob, picture \| sound, name)` | Adds a TimelineMobSlot:<br>• SlotID = highest + 1;<br>• the edit rate of the mob's first timeline slot;<br>• Origin 0;<br>• PhysicalTrackNumber = count of tracks of that kind + 1;<br>• an empty Sequence referencing the file's Picture or Sound DataDefinition (SMPTE AUID preferred, legacy accepted). |
| `removeTrack(slot)` | Deletes the slot. |
| `addMarker(mob, position, comment)` | Adds a DescriptiveMarker (Position, Comment, the marker track's DataDefinition, and **DescribedSlots = {first picture track's SlotID}**), in position order. If there is no marker track, it creates an EventMobSlot (PhysicalTrackNumber = existing event slots + 1, the first timeline edit rate) holding a Sequence that references the Descriptive Metadata DataDefinition. Avid does the same, and the OTIO adapter requires both `DescribedSlots` and the event slot's PhysicalTrackNumber. |
| `relink(find, replace)` | Replaces text in every NetworkLocator `URLString`; returns how many URLs actually changed. |
| `deepCopy(id)` | Copies an object with its strongly referenced children. Sets are refused. |

- **Not yet supported**: moving or trimming transitions, editing inside effects and nested scopes, and keyframes.
- **Tests** (`tests/timeline/test_edit_ops.cpp`, on the SDK sample so they run on every PR):
  - every operation, including refusals;
  - the stored Length always equal to the projected total;
  - no adjacent fillers;
  - 0 validation errors;
  - exact undo;
  - a random sequence of 80 operations that is undone to the original objects.
- **Cross-check** (`tools/crosscheck_edits.py`, nightly): split, overwrite, add-track and add-marker are applied through `aaftool rpc` to every reference file, and the result is saved. The saved file must validate with 0 errors, pyaaf2 must read the same mobs, and OTIO must read every comparable edited track identically to our projection. All 43 files pass.

## 7. Layer 4: Edit session (`libaafedit`)

Status: **implemented (M4)** in `libs/edit/` (namespace `aaf::edit`, headers `aaf/edit/{transaction,operations,session}.hpp`).

- **Transactions**: every change runs inside a `Transaction`.
  - `touch(id)` snapshots an object the first time it is handed out for mutation.
  - `create(class)` appends a detached object, with a blank snapshot.
  - `referencedProperties()` snapshots the tag table.
  - `rollback()` restores every snapshot. `commit()` produces a `Journal`: the before and after states of every object that actually changed, the created ids, and the before and after tag tables.
  - Undo applies the before states and redo applies the after states. Commands therefore need no hand-written revert logic, and composite commands are simply functions that call several primitives in one transaction.
- **Primitives** (`operations.hpp`, all taking a `Transaction&`):

  | Operation | Rules |
  |---|---|
  | `setProperty(id, pid, Value)` | The property must be defined for the class and stored as data. The value is encoded with the property type in the object's byte order, and must be at most 65535 bytes. If the property is the key of the set the object belongs to, the set entry key is updated (and must stay unique), and **every weak reference with that set's tag and the old key is rewritten**. |
  | `removeProperty(id, pid)` | Required properties cannot be removed. Strongly referenced children are detached, but only if nothing weakly references them. |
  | `createObject(class)` | The class must be known and concrete. The object is created detached. |
  | `setStrongRef(parent, pid, child)` | The child must be detached, not the root, and not an ancestor of the parent (no cycles). Its class must match the reference's class. The previous child, which must be unreferenced, is detached. The storage name is kept, or generated for a new property. |
  | `ensureCollection(parent, pid)` | Creates an empty strong vector or set, as required for mandatory collections such as `Mob.Slots`. For sets, the key PID and size come from the element class's unique-identifier property. |
  | `insertIntoCollection(parent, pid, index, child)` | Vectors insert at `index` and take the local key `firstFreeKey++`. Sets append an entry `{firstFreeKey++, refCount 1, key}`: the child's unique-identifier bytes, which must be set and unique. |
  | `removeFromCollection(parent, pid, index)` | Returns the detached child, so it can be reinserted in the same transaction to move it. |
  | `moveInCollection(parent, pid, from, to)` | Vectors only. Local keys move with their elements. |
  | `deleteObject(id, force)` | Detaches the object from its parent, whether a singleton, vector or set. Without `force`, fails if any weak reference points into the subtree. With `force`, dangling references are allowed. |
  | `setWeakRef(id, pid, target)` | The target must be attached, an element of a strong set, and of the referenced class. Its tag is found from the path of `parentPid`s from the root, or appended to `referenced properties`. The key is the target's set entry key. |
  | `setStreamData(id, pid, bytes \| shared ByteSource)` | Replaces or adds stream contents. A shared `ByteSource` (for example a file) is read lazily on save and must stay readable until then. |
  | `pidOf(doc, id, name)` | Looks up a property PID by name, including superclasses. |

  Weak-reference collections (vectors and sets of weak references) are not yet editable.
- **Session** (`Session`): owns the `Document`, the undo history and a listener.
  - **`execute(description, command)`**:
    1. Runs the command in a transaction.
    2. Rebuilds the indexes.
    3. **Validates** every touched object that is attached: any error from `validateObject` rejects the command.
    4. Unless the command was forced, rejects it if the number of unresolved weak references increased.
    5. On any failure, rolls back, so **the document is unchanged**.
    6. On success, commits, truncates the redo history, pushes the step and notifies the listener.

    Commands that change nothing are not recorded.
  - **`undo()` and `redo()`** apply journals. `history()` and `position()` expose the history.
  - **`dirty()`** is true when the position differs from the saved one. After truncating past the saved position, the document stays dirty until it is saved.
  - **`save(path, options)`** keeps the history.
- **`ChangeSet`**: the objects that changed, the created ids, the individual `{object, pid}` property changes, and whether the tag table changed. The UI uses it to refresh only what is affected.
- **Tests** (`tests/edit/`):
  - every primitive, including rejection cases and atomic rollback;
  - building a complete new CompositionMob: a slot, a sequence, a filler and weak references to a DataDefinition;
  - re-identifying a DataDefinition that is weakly referenced, with all references still resolving;
  - forced and unforced deletion;
  - vector moves;
  - stream replacement;
  - save and dirty tracking;
  - **random edit sequences on two fixtures**: 60 steps each, validating with 0 errors after every step. Undoing everything restores every object exactly, and a save then gives a container tree identical to the original file's. Redoing everything restores the edited state exactly.

## 8. Applications

### 8.1 `aaftool` (CLI)

```
aaftool dump <file> [--json] [--depth N] [--header|--metadict]
                                              object tree from the root, Header or MetaDictionary (M2)
aaftool cfb <file>                            raw CFB directory listing (header info + tree)
aaftool cfb-roundtrip <in> <out> [--v3|--v4]  rewrite the CFB container only (no AAF parsing)
aaftool validate <file> [--json]              diagnostics; exit 1 on errors (M2)
aaftool roundtrip <in> <out> [--v3|--v4] [--regenerate-layout]
                                              load and save without edits (M3)
aaftool timeline <file> [--mobs] [--mob NAME|ID] [--json]
                                              mob list, or a mob's tracks and items (M6)
aaftool rpc <file> (--call METHOD PARAMS)... [--save OUT]
                                              run any §8.3 RPC calls in order, printing each result, then optionally save (M7)
aaftool extract <file> --list                 list embedded essence: index, MobID, size, mob name
aaftool extract <file> <mobid|index> <out>    write an embedded essence stream to a file, in 1 MiB chunks
aaftool set-essence <in> <mobid|index> <data> <out>
                                              replace an embedded essence stream with a file's contents (file-backed, undoable edit)
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

### 8.2 Editor host (`apps/editor`, executable `aafedit`)

Status: **implemented (M5)**.

- **Command line**: `aafedit [file.aaf] [--debug] [--smoke-test file.aaf]`. `--debug` enables the web inspector.
- **Webview**: [webview/webview](https://github.com/webview/webview) 0.12.0 (WebKitGTK 4.1 on Linux, WKWebView on macOS, WebView2 on Windows, using its built-in loader, so no extra DLL is needed).
  - It is built as webview's **static library**, compiled as C++17: its header-only C++ implementation does not compile under Clang in C++26 mode.
  - The host uses only webview's **C API**, through a small RAII wrapper (`view.hpp`: `View` with `setTitle`, `setSize`, `setHtml`, `init`, `eval`, `bind`, `resolve`, `dispatch`, `run` and `terminate`).
  - On Linux the editor is built only when `pkg-config` finds `webkit2gtk-4.1`, and otherwise skipped with a warning. The option `AAF_BUILD_EDITOR` (default ON) controls it.
- **UI embedding**:
  - CMake runs `npm ci && npm run build` in `ui/` (Vite and `vite-plugin-singlefile`), producing one self-contained `index.html` of about 85 KB.
  - `tools/embed_file.py` turns it into `generated/ui_html.cpp`, which exposes `aaf::embedded::indexHtml() -> std::string_view` and is marked NOLINT.
  - The host loads the page with `setHtml`. `AAF_UI_DIST=<dir>` uses a prebuilt `index.html` instead, for machines without npm.
  - The page's CSP is `default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; connect-src 'none'`: no network access at all.
- **Threads**:
  - The JSON-RPC server runs on a single worker thread (`Worker`, a task queue on a `std::jthread`), so the UI thread never blocks on file I/O.
  - `aafRpc` calls are posted to the worker, and `webview_return` (which is thread-safe) completes them.
  - Server events are pushed with `dispatch(eval("window.__aafEvent(...)"))`.
  - A file given on the command line is opened by the first worker task, before any UI request is served.
- **Host commands** (`aafHost(command, options)`, run on the UI thread):
  - `openDialog({data?})` and `saveDialog({suggested?, data?})` use [portable-file-dialogs](https://github.com/samhocevar/portable-file-dialogs), pinned by commit, which is native on Windows and macOS and uses zenity or kdialog on Linux. They return a path or `null`.
  - `setTitle({title})`.
  - `quit({code, message?})`.
  - There are no native menus: the UI toolbar and keyboard shortcuts cover every action.
- **Smoke test** (`--smoke-test file`):
  - The host injects `window.__aafSmoke = {file}` with `init`.
  - The UI then runs `ui/src/lib/smoke.ts` over the real bridge: open, list the root's children, search for mobs, rename a named mob and check the rename, undo, and validate with 0 errors.
  - It reports through `quit`, and the process exit code is the result.
  - A 60-second watchdog exits with code 3 if the page never reports.
  - CI runs it on Linux (under `xvfb-run`), Windows and macOS.
- **Not yet handled**: a prompt for unsaved changes when the window is closed (webview has no close hook), and native menus.

### 8.3 RPC bridge (`apps/rpc`, library `aaf::rpc`)

Status: **implemented (M5)**. The bridge is a standalone library (`Server`), tested without any window. It depends on nlohmann/json, so it lives under `apps/`.

- **Protocol**: JSON-RPC 2.0. Requests without an `id` are notifications and get no response. Error codes are:
  - `-32700`: parse error;
  - `-32600`: invalid request;
  - `-32601`: unknown method (checked before anything else);
  - `-32602`: missing parameter;
  - `-32000`: application error, with the message and `data.kind` (the `aaf::Errc` name).
- **Transport**: the page calls `window.aafRpc(requestText)`, and the promise resolves with the response object. Object IDs and PIDs are JSON numbers, because they are always far below 2⁵³.
- **Methods** (parameters are objects; results are described after the arrow):

  | Method | Parameters → result |
  |---|---|
  | `doc.info` | → `{open, path, name, dirty, canUndo, canRedo, undo, redo, objectCount, version, root, header, metaDictionary}` |
  | `doc.open` | `{path}` → info |
  | `doc.close` | → info |
  | `doc.save` | → info |
  | `doc.saveAs` | `{path, regenerateLayout?, version? (3 or 4)}` → info. Later saves go to the new path. |
  | `doc.validate` | → `[{severity, object, pid, property, message}]` |
  | `tree.children` | `{id, offset=0, limit=500}` → `{total, items:[{id, class, label, pid, property, index, key, childCount}]}`. Children are the strongly referenced objects in property order. `key` is the set key. |
  | `tree.path` | `{id}` → object IDs from the root to `id` |
  | `object.get` | `{id}` → `{id, class, classId, concrete, label, parent, parentPid, attached, properties, available, types}` (see below) |
  | `object.setProperty` | `{id, pid, value}` → change set |
  | `object.removeProperty` | `{id, pid}` → change set |
  | `object.create` | `{parent, pid, class (name or AUID), index?}` → `{id, changes}`. Creates the object with `createWithDefaults`, then sets or inserts it (appends if no index). |
  | `object.delete` | `{id, force?}` → change set |
  | `object.move` | `{parent, pid, from, to}` → change set |
  | `object.setWeakRef` | `{id, pid, target}` → change set |
  | `object.candidates` | `{id, pid}` → the objects a weak reference may target |
  | `model.subclasses` | `{class}` → the concrete subclasses |
  | `edit.undo`, `edit.redo` | → change set |
  | `edit.history` | → `{items, position}` |
  | `search.query` | `{text?, class?, limit=200}` → `[{id, class, label}]`. Matches the label or any string, AUID or MobID data value, case-insensitively, among attached objects. |
  | `essence.extract` | `{id, path}` → `{size}` |
  | `essence.replace` | `{id, path}` → change set (file-backed) |

  - Each property in `object.get` is `{pid, name, kind, storedForm, type, optional, uniqueId, …}`, plus a kind-specific payload:
    - `data`: `value` (a tagged value), or `error` together with the raw bytes;
    - `strongRef`: `children`;
    - vectors and sets: `count`;
    - `weakRef`: `target {id|null, key, label, resolved?}`;
    - weak collections: `targets`;
    - `stream`: `size`.
  - `available` lists defined properties that are absent. `types` holds every referenced type descriptor (`{id, name, kind, element?, className?, size?, signed?, count?, fields?, elements?}`), so the UI can build editors without further calls.
  - Timeline (M6):
    - `timeline.mobs` returns the mob summaries.
    - `timeline.get {mob}` returns `{mob, mobId, name, kind, timecode, warnings, tracks:[{slot, slotId, name, physicalNumber, kind, slotKind, editRate:{num, den}, origin, length, segment, effects:[{object, name}], items}]}`, with each item's fields as in §6 (`nested` is a list of item lists).
    - `timeline.resolve {clip}` returns `{status, links, essence}`.
    - `timeline.op {op, …}` (M7) runs a §6.1 operation as one undoable step and returns `{changes, id?, count?}`. Operations:
      - `split {slot, position}` → `id` of the right part;
      - `lift {item}` and `rippleDelete {item}`;
      - `trim {item, edge: head|tail, delta, ripple?}`;
      - `move {item, toSlot, position, ripple?}`;
      - `insertClip` and `overwriteClip {slot, position, sourceMob, sourceSlot, sourceIn, length}` → `id`;
      - `addTrack {mob, kind: picture|sound, name?}` → `id`;
      - `removeTrack {slot}`;
      - `addMarker {mob, position, comment?}` → `id`;
      - `relink {find, replace}` → `count`.
- **Events**: `doc.opened` (info), `doc.changed` (`{changes, info}`, from the session listener) and `doc.state` (info, after a save).
- **Tagged values** (`toJson` and `valueFromJson`): `{"t":…}` with one of these tags:
  - `null`; `bool`;
  - `int` and `uint` (decimal **strings**);
  - `string`, `auid`, `mobid` (the URN);
  - `enum` (`v` as a string, plus `name`) and `extenum` (`v` as an AUID, plus `name`);
  - `record` (`fields:[{name, value}]`) and `array` (`items`);
  - `indirect` (`type`, `value`) and `opaque` (`type`, `bytes` in hex);
  - `bytes` (`v` in hex).

  A round-trip test covers every tag. The TypeScript mirror is `ui/src/lib/rpc.ts`.
- `labelOf(doc, id)` gives the `Name` property if it is set, else the unique identifier, else the class name.

### 8.4 UI (`ui/`)

Status: **tree and inspector (M5), read-only timeline (M6)**; timeline editing is M7.

- **Stack**: TypeScript 5.9 (svelte-check does not support TypeScript 7 yet), Svelte 5 (runes), Vite 8 with `vite-plugin-singlefile`, and Vitest 5 on jsdom. Versions are pinned in `ui/package.json` and the lockfile. `npm run check` runs `svelte-check --fail-on-warnings`, and `npm test` runs Vitest.
- **Modules**:
  - `lib/rpc.ts`: `RpcClient` with typed methods and an injectable transport, `hostCommand`, and the protocol types.
  - `lib/values.ts`: editor kind per type, formatting, parsing with range checks (BigInt for 64-bit values), client-side defaults that mirror the server's, and immutable record and array updates.
  - `lib/tree.ts`: `TreeModel`, a lazy, paged tree (200 per page) with `expand`, `collapse`, `loadMore`, `reveal(path)` and `refresh(changed)`. A refresh reloads changed objects and their parents' child lists, keeping expansion state.
  - `lib/smoke.ts`.
  - `lib/components/`: `TreeView`, `PropertyPanel`, `ValueEditor` and `BottomPanel`.
- **Layout**:
  - A toolbar: Open, Save, Save As, Undo and Redo (whose tooltips name the step), search, and the file name with a dirty marker, object count and version.
  - The object tree on the left, virtualised (24 px rows, only visible rows rendered), with search results above it.
  - The property panel on the right.
  - A bottom panel with tabs for Diagnostics (Validate, with each entry linking to its object) and History (click an entry to undo or redo to that point).
  - A single column below 700 px width.
- **Property panel**:
  - Header actions: Parent, Move up and Move down (vectors only), and Delete. Delete offers "delete anyway" when weak references would dangle.
  - Type-aware editors:
    - checkbox for booleans;
    - dropdowns for enumerations and extendible enumerations, showing undefined values explicitly;
    - text inputs for integers (range-checked) and strings;
    - monospace inputs with a copy button for AUIDs and MobIDs;
    - nested editors for records;
    - element editing and add/remove for variable arrays;
    - read-only display for opaque, indirect and undecodable values.
  - References: strong references are links, and collections have an Add button that asks for a concrete subclass when there are several. Weak references show a link plus a "Change…" dropdown of candidates.
  - Streams show their size, with Extract and Replace buttons for essence.
  - Optional properties can be removed, and missing ones added. Missing required properties are flagged.
  - Edits commit on Enter or blur; Escape reverts.
- **Timeline** (`TimelineView`, drawn on `<canvas>`, M6):
  - **Opening**: a Timelines dropdown in the toolbar (top-level compositions are starred), a Timeline button on mob objects, and the first top-level composition opens with the file. Open timelines are tabs above the property panel. The panel is split roughly 50/50, and both views stay in sync.
  - **Layout** (`timelineLayout.ts`):
    - rows ordered picture, sound, events, then the rest;
    - labels V1…, A1…, M1… for markers, TC/EC/DM/D for code and data tracks (ordinal numbers);
    - heights of 38 px for media, 26 px for events and 18 px otherwise.
    - Every track is scaled to the **base rate** (the first picture timeline track, else the first timeline track), so audio at 48 kHz lines up with 24 fps video.
  - **Ruler**: timecode from the mob's timecode start, fps and drop-frame flag (`timecode.ts`: SMPTE drop-frame formatting and parsing, tested). Ticks are spaced at least 90 px apart, stepping through 1/2/5/10 frames, then 1 s … 1 h.
  - **Drawing**:
    - clips coloured by kind: video, audio, effect, code, and nested for NestedScope or clips of compositions;
    - fillers as dashed outlines;
    - transitions as orange boxes with an X;
    - markers as diamonds;
    - **clips with missing sources in red**;
    - the selected object outlined;
    - track headers showing name, rate or track effects.
    - It scales for the device pixel ratio and reads colours from CSS variables, so both themes work.
  - **Interaction**:
    - Ctrl+wheel or +/− to zoom around the pointer, and Fit;
    - wheel or Shift+wheel, and a range slider, to scroll;
    - hover tooltips (kind, label, start and length, source reference, comment);
    - click to select: the object is revealed in the tree and shown in the properties. For source clips the resolved chain appears in the header: mobs, status, and embedded or first locator.
- **Timeline editing** (M7):
  - **Playhead**: click the ruler; Left and Right move it by 1 unit, or 10 with Shift. The ruler shows its timecode.
  - **Selection**: click a track header to select the track; clicking an item selects both it and its track.
  - **Drag**:
    - dragging a clip body moves it, including onto another track of the same kind, snapping within 8 px to every edit point and the playhead; Alt makes it a ripple move;
    - dragging within 6 px of an edge trims it, as a roll, or a ripple with Alt;
    - a dashed ghost shows the result, and the cursor changes over edges.
  - **Toolbar**: Split (S), Lift (Delete), Ripple (Shift+Delete), Marker (M, asks for a comment), a source picker with Insert and Overwrite at the playhead on the selected track (using the whole matching source track), +V, +A, −Track and Relink… (find and replace).
  - Keyboard shortcuts act only while the timeline is active (after a click inside it).
  - Errors from refused operations appear in the message bar.
  - After a change, the selection is cleared if the selected object left the document.
- **Keyboard**:
  - Ctrl/Cmd+O, S, Shift+S;
  - Z and Shift+Z or Y (except inside text fields);
  - F for search;
  - arrow keys, Enter, Left and Right in the tree.
- **Themes**: CSS custom properties with light and dark themes that follow `prefers-color-scheme`.

## 9. Fidelity requirements (acceptance criteria)

1. **Lossless round-trip**: for every fixture, `open → save` without edits yields a file that re-opens to a semantically identical graph: the same classes, property values, collection order, weak-ref targets and stream bytes. Byte-identical output is *not* required. ✅ M3: with the default layout preservation, the container tree is identical to the source for all 58 reference files at both sector sizes. With regenerated layout, the graph is semantically identical and validates with 0 errors.
2. Unknown classes, properties and opaque data survive round-trip. ✅ M3: a test adds an unknown stored form, a vendor storage and a root `SummaryInformation` stream, and all of them survive.
3. Files written by us open in: pyaaf2 (automated in CI), the AAF SDK InfoDumper (automated if feasible), and Avid Media Composer, Pro Tools and DaVinci Resolve (manual release checklist). ✅ pyaaf2 (M3): `tools/crosscheck.py --roundtrip` passes for all 58 files in both layout modes. InfoDumper is not automated: it would require building the SDK, which is not worth the CI cost.
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
  - Essence: `tools/crosscheck.py --essence <aaftool>` checks, for every reference file, that every stream written by `aaftool extract` equals pyaaf2's reading of it, and that pyaaf2 reads back a stream replaced by `aaftool set-essence`. The largest case is 4.7 MB of DNxHD in `picchu_seq0100_snippet_embedded.aaf`.
  - Round-trip (M3): `tools/crosscheck.py --roundtrip <aaftool>` rewrites each file with `aaftool roundtrip`, both preserving the layout (as v3) and regenerating it (as v4). It then compares pyaaf2's reading of the output with aaftool's reading of the original. Stream names are ignored for regenerated layouts.
- Python tool versions are pinned in `tools/requirements.txt` (pyaaf2 1.7.1, pytest).
- **Corruption**: unit tests cover a bad header, truncation, FAT cycles, directory cycles and 900 deterministic random mutations. `tests/fuzz/fuzz_cfb.cpp` (libFuzzer: open, read every stream, rewrite) and `tests/fuzz/fuzz_document.cpp` (load an AAF document, decode every data property, validate, then save it and require that the output reloads) each run 10 minutes nightly, seeded from the SDK fixtures. The M2 baseline was 16.7k document executions in 5 minutes with no findings; each input is a whole AAF file. The M1 baseline was 1.48 M executions in 5 minutes with no findings.
- **UI**: Vitest for the frontend logic and the RPC codec. Playwright smoke test against a headless build of the UI with a mock bridge.

## 11. CI/CD (GitHub Actions, frugal)

- **`ci.yml`** (every PR and push to main):
  - lint: clang-format 22 check, pytest for `tools/`, the generated-model check, and the UI's `svelte-check` and Vitest (Node 24);
  - Linux GCC 14 Debug and Clang 22 Release, with `-Werror`, unit tests, and clang-tidy (`.clang-tidy`, warnings as errors) on the Clang job;
  - Windows MSVC and macOS AppleClang Release builds and tests;
  - every build job (both Linux jobs, Windows and macOS) builds `aafedit` and runs its `--smoke-test` (Linux under `xvfb-run`).
- The sanitizer, TSan and fuzz jobs build without the editor (`AAF_BUILD_EDITOR=OFF`).
  - LLVM 22 comes from apt.llvm.org (`.github/actions/setup-llvm`).
- **`full.yml`** (push to main, nightly at 03:17 UTC, manual, or PRs that change it):
  - ASan/UBSan tests with the external fixtures (cached by manifest hash), and the pyaaf2 cross-checks (object, round-trip and essence);
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
| M3 ✅ | Stored-format writer, `aaftool roundtrip` | §9.1–9.3 (automated parts) pass |
| M4 ✅ | Edit session + primitive commands | Undo/redo symmetry tests pass |
| M5 ✅ | Webview host, RPC, tree and property inspector | Edit and save any property from the UI |
| M6 ✅ | Timeline projection + read-only timeline view | Fixtures render correctly, with selection sync |
| M7 ✅ | Timeline editing ops (§6.1) | Op tests pass; edited files validate and read back identically in pyaaf2 and OTIO (automated). Resolve and Pro Tools stay on the manual release checklist. |
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
| 2026-09-27 | The writer keeps each object's byte order instead of forcing little-endian, so unmodified data is written back exactly as read |
| 2026-09-27 | Windows source files are opened with delete sharing so that saving over the open source works |
| 2026-09-27 | Baseline property types win over conflicting file declarations |
| 2026-09-27 | Undo/redo uses object snapshots (before and after journals) instead of per-command inverse operations |
| 2026-09-27 | Changing a set element's key rewrites all weak references to it within the same command |
| 2026-09-27 | Windows and macOS builds run on every PR; sanitizers, external fixtures and fuzzing stay nightly |
| 2026-09-27 | Edited stream data is a shared `ByteSource`, so large essence can be replaced from a file without loading it |
| 2026-09-27 | webview is used through its C API, built as a C++17 static library; its header-only C++ implementation does not compile in C++26 |
| 2026-09-27 | The UI is built to a single inline HTML file loaded with `set_html`: no local server, no network |
| 2026-09-27 | The RPC server is a separate library (`apps/rpc`), tested headlessly; the host only relays calls |
| 2026-09-27 | UI actions live in a toolbar and keyboard shortcuts instead of native menus |
| 2026-09-27 | Track-level effects are unwrapped in the timeline; nested compositions and scopes stay single items |
| 2026-09-27 | The timeline projection is verified against the OpenTimelineIO AAF adapter's reading of its own sample files |
| 2026-09-27 | Removing a transition makes a cut at its midpoint, preserving timing |
| 2026-09-27 | New markers carry DescribedSlots, and new marker slots a PhysicalTrackNumber, as Avid writes them (OTIO requires both) |

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
