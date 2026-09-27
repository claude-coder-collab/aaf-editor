#pragma once

#include <aaf/cfb/byte_io.hpp>
#include <aaf/cfb/types.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aaf::cfb
{

/// A directory entry of an opened compound file. `children` are in directory order (sorted by `compareNames`).
struct DirEntry
{
    EntryId id = kNoStream;
    EntryId parent = kNoStream;
    std::u16string name;
    EntryType type = EntryType::empty;
    Clsid clsid;
    std::uint32_t stateBits = 0;
    std::uint64_t creationTime = 0;
    std::uint64_t modifiedTime = 0;
    std::uint32_t startSector = kEndOfChain;
    std::uint64_t size = 0;
    std::vector<EntryId> children;

    [[nodiscard]] auto isStorage() const noexcept -> bool { return type == EntryType::storage || type == EntryType::root; }
    [[nodiscard]] auto isStream() const noexcept -> bool { return type == EntryType::stream; }
};

/// Header fields of an opened compound file.
struct HeaderInfo
{
    Version version = Version::v4;
    std::uint16_t minorVersion = 0x3E;
    /// Header CLSID (offset 8). [MS-CFB] requires zero, but AAF stores its file signature here.
    Clsid clsid;
    std::uint32_t sectorSize = 4096;
    std::uint32_t fatSectors = 0;
    std::uint32_t miniFatSectors = 0;
    std::uint32_t difatSectors = 0;
    std::uint32_t directorySectors = 0;
};

/// Random-access reader for one stream. Holds a non-owning pointer to the container's source,
/// so it must not outlive the `Container` it was opened from.
class StreamReader
{
public:
    StreamReader() = default;
    StreamReader(const ByteSource* source, std::uint64_t size, std::uint32_t unitSize, std::vector<std::uint64_t> unitOffsets);

    [[nodiscard]] auto size() const noexcept -> std::uint64_t { return size_; }
    /// Reads up to `out.size()` bytes at `offset`; returns the number read (short only at end of stream).
    [[nodiscard]] auto read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t>;
    /// Reads the whole stream; fails with `Errc::limit` if it is larger than `maxSize`.
    [[nodiscard]] auto readAll(std::uint64_t maxSize = std::uint64_t{ 1 } << 32) const -> Result<std::vector<std::byte>>;

private:
    const ByteSource* source_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint32_t unitSize_ = 0;
    std::vector<std::uint64_t> unitOffsets_;
};

/// A read-only view of a compound file ([MS-CFB] v3 or v4). All structure is validated on open;
/// stream data is read lazily from the source.
class Container
{
public:
    [[nodiscard]] static auto open(std::unique_ptr<ByteSource> source) -> Result<Container>;
    [[nodiscard]] static auto openFile(const std::filesystem::path& path) -> Result<Container>;

    [[nodiscard]] auto header() const noexcept -> const HeaderInfo& { return header_; }
    [[nodiscard]] auto root() const noexcept -> const DirEntry& { return entries_.front(); }
    [[nodiscard]] auto entry(EntryId id) const -> const DirEntry& { return entries_.at(id); }
    /// All directory slots, indexed by `EntryId`. Slots not reachable from the root have type `empty`.
    [[nodiscard]] auto entries() const noexcept -> std::span<const DirEntry> { return entries_; }
    [[nodiscard]] auto find(EntryId parent, std::u16string_view name) const -> std::optional<EntryId>;
    /// Looks up a '/'-separated UTF-8 path relative to the root.
    [[nodiscard]] auto findPath(std::string_view path) const -> std::optional<EntryId>;
    [[nodiscard]] auto openStream(EntryId id) const -> Result<StreamReader>;
    [[nodiscard]] auto source() const noexcept -> const ByteSource& { return *source_; }

private:
    Container() = default;
    [[nodiscard]] auto sectorOffset(std::uint32_t sector) const noexcept -> std::uint64_t;
    [[nodiscard]] auto readSectors(std::span<const std::uint32_t> chain, std::uint64_t maxBytes) const -> Result<std::vector<std::byte>>;
    [[nodiscard]] auto loadFat(std::span<const std::byte> header) -> Result<void>;
    [[nodiscard]] auto loadDirectory(std::uint32_t firstSector) -> Result<void>;
    [[nodiscard]] auto loadMiniStream(std::uint32_t firstMiniFatSector) -> Result<void>;

    std::unique_ptr<ByteSource> source_;
    HeaderInfo header_;
    std::vector<std::uint32_t> fat_;
    std::vector<std::uint32_t> miniFat_;
    std::vector<std::uint32_t> miniStreamChain_;
    std::vector<DirEntry> entries_;
};

/// Follows a sector chain from `start` through `table`, rejecting out-of-range entries and cycles.
[[nodiscard]] auto followChain(std::span<const std::uint32_t> table, std::uint32_t start) -> Result<std::vector<std::uint32_t>>;

}
