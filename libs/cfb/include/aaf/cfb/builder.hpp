#pragma once

#include <aaf/cfb/byte_io.hpp>
#include <aaf/cfb/container.hpp>
#include <aaf/cfb/types.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace aaf::cfb
{

/// Stream contents taken lazily from a stream of an existing container, which must outlive the write.
struct SourceStream
{
    const Container* container = nullptr;
    EntryId id = kNoStream;
};

/// Stream contents read from any byte source, for example a file on disk; shared so edits can hold it cheaply.
struct SharedSource
{
    std::shared_ptr<const ByteSource> source;
};

using StreamData = std::variant<std::vector<std::byte>, SourceStream, SharedSource>;

/// Size of stream data.
[[nodiscard]] auto sizeOf(const StreamData& data) -> std::uint64_t;
/// Reads up to `out.size()` bytes of stream data at `offset`.
[[nodiscard]] auto readStreamData(const StreamData& data, std::uint64_t offset, std::span<std::byte> out) -> Result<std::size_t>;

using NodeId = std::uint32_t;

/// A node of the tree being built. Node 0 is always the root storage.
struct BuildNode
{
    std::u16string name;
    EntryType type = EntryType::storage;
    Clsid clsid;
    std::uint32_t stateBits = 0;
    std::uint64_t creationTime = 0;
    std::uint64_t modifiedTime = 0;
    StreamData data;
    std::vector<NodeId> children;
};

/// Describes a compound file to be written: a tree of storages and streams.
class Builder
{
public:
    explicit Builder(Version version = Version::v4);

    /// Copies the whole tree of `source`. Stream data is referenced, not copied, so `source` must outlive the write.
    [[nodiscard]] static auto fromContainer(const Container& source) -> Result<Builder>;

    [[nodiscard]] auto version() const noexcept -> Version { return version_; }
    void setVersion(Version version) noexcept { version_ = version; }
    /// CLSID written to the file header (offset 8); AAF uses it as the file signature.
    [[nodiscard]] auto headerClsid() const noexcept -> const Clsid& { return headerClsid_; }
    void setHeaderClsid(const Clsid& clsid) noexcept { headerClsid_ = clsid; }

    [[nodiscard]] static constexpr auto root() noexcept -> NodeId { return 0; }
    [[nodiscard]] auto addStorage(NodeId parent, std::u16string name, Clsid clsid = {}) -> Result<NodeId>;
    [[nodiscard]] auto addStream(NodeId parent, std::u16string name, StreamData data) -> Result<NodeId>;

    [[nodiscard]] auto node(NodeId id) -> BuildNode& { return nodes_.at(id); }
    [[nodiscard]] auto node(NodeId id) const -> const BuildNode& { return nodes_.at(id); }
    [[nodiscard]] auto nodeCount() const noexcept -> std::size_t { return nodes_.size(); }
    [[nodiscard]] auto streamSize(NodeId id) const -> std::uint64_t;

private:
    [[nodiscard]] auto addNode(NodeId parent, BuildNode node) -> Result<NodeId>;

    Version version_;
    Clsid headerClsid_;
    std::vector<BuildNode> nodes_;
};

/// Serialises `builder` as a compound file.
[[nodiscard]] auto write(const Builder& builder, ByteSink& sink) -> Result<void>;

/// Serialises `builder` to `path` atomically (see `writeFileAtomic`).
[[nodiscard]] auto writeFile(const Builder& builder, const std::filesystem::path& path) -> Result<void>;

namespace detail
{

/// Arranges `count` sorted siblings as a balanced binary tree coloured as a valid red-black tree.
struct TreeLayout
{
    std::vector<std::uint32_t> left;
    std::vector<std::uint32_t> right;
    std::vector<bool> red;
    std::uint32_t root = kNoStream;
};

[[nodiscard]] auto layoutSiblings(std::uint32_t count) -> TreeLayout;

}

}
