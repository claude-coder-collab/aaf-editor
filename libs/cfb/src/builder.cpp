#include "endian.hpp"

#include <aaf/cfb/builder.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>

namespace aaf::cfb
{

namespace
{

using detail::storeLE;

constexpr std::size_t kCopyChunk = std::size_t{ 1 } << 20;
constexpr std::uint64_t kMaxV3StreamSize = 0x80000000;

auto ceilDiv(std::uint64_t a, std::uint64_t b) -> std::uint64_t
{
    return (a + b - 1) / b;
}

struct Slot
{
    NodeId node = 0;
    std::uint32_t left = kNoStream;
    std::uint32_t right = kNoStream;
    std::uint32_t child = kNoStream;
    bool red = false;
    std::uint32_t start = kEndOfChain;
    std::uint64_t size = 0;
};

class Writer
{
public:
    Writer(const Builder& builder, ByteSink& sink) :
        builder_(builder),
        sink_(sink),
        ss_(sectorSize(builder.version())),
        perSector_(ss_ / 4)
    {
    }

    auto run() -> Result<void>
    {
        if (auto r = planDirectory(); !r)
        {
            return r;
        }
        if (auto r = planStreams(); !r)
        {
            return r;
        }
        planTables();
        return emit();
    }

private:
    auto planDirectory() -> Result<void>
    {
        if (builder_.node(Builder::root()).type != EntryType::storage)
        {
            return fail(Errc::invalid_argument, "root node must be a storage");
        }
        slots_.push_back(Slot{ .node = Builder::root() });
        std::vector<std::uint32_t> pending = { 0 };
        while (!pending.empty())
        {
            const std::uint32_t dirId = pending.back();
            pending.pop_back();
            const auto& parent = builder_.node(slots_[dirId].node);
            std::vector<NodeId> sorted = parent.children;
            std::ranges::sort(sorted, [&](NodeId a, NodeId b) -> bool { return compareNames(builder_.node(a).name, builder_.node(b).name) < 0; });
            for (std::size_t i = 1; i < sorted.size(); ++i)
            {
                if (compareNames(builder_.node(sorted[i - 1]).name, builder_.node(sorted[i]).name) == 0)
                {
                    return fail(Errc::invalid_argument, std::format("duplicate entry name '{}'", toUtf8(builder_.node(sorted[i]).name)));
                }
            }
            const auto first = static_cast<std::uint32_t>(slots_.size());
            const auto count = static_cast<std::uint32_t>(sorted.size());
            const auto layout = detail::layoutSiblings(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto map = [first](std::uint32_t v) -> std::uint32_t { return v == kNoStream ? kNoStream : first + v; };
                slots_.push_back(Slot{ .node = sorted[i], .left = map(layout.left[i]), .right = map(layout.right[i]), .red = layout.red[i] });
            }
            slots_[dirId].child = count == 0 ? kNoStream : first + layout.root;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (builder_.node(sorted[i]).type == EntryType::storage)
                {
                    pending.push_back(first + i);
                }
            }
        }
        return {};
    }

    auto planStreams() -> Result<void>
    {
        for (auto& slot : slots_)
        {
            const auto& node = builder_.node(slot.node);
            if (node.type != EntryType::stream)
            {
                slot.start = slot.node == Builder::root() ? kEndOfChain : 0;
                continue;
            }
            slot.size = builder_.streamSize(slot.node);
            if (builder_.version() == Version::v3 && slot.size > kMaxV3StreamSize)
            {
                return fail(Errc::limit, std::format("stream '{}' is too large for a version 3 file", toUtf8(node.name)));
            }
            if (slot.size == 0)
            {
                slot.start = kEndOfChain;
            }
            else if (slot.size < kMiniStreamCutoff)
            {
                auto data = streamBytes(node);
                if (!data)
                {
                    return std::unexpected(data.error());
                }
                const auto first = static_cast<std::uint32_t>(miniFat_.size());
                const auto count = static_cast<std::uint32_t>(ceilDiv(slot.size, kMiniSectorSize));
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    miniFat_.push_back(i + 1 == count ? kEndOfChain : first + i + 1);
                }
                slot.start = first;
                miniStream_.insert(miniStream_.end(), data->begin(), data->end());
                miniStream_.resize(std::size_t{ miniFat_.size() } * kMiniSectorSize);
            }
            else
            {
                slot.start = static_cast<std::uint32_t>(nextSector_);
                bigStreams_.push_back(static_cast<std::uint32_t>(&slot - slots_.data()));
                nextSector_ += ceilDiv(slot.size, ss_);
            }
        }
        return {};
    }

    void planTables()
    {
        auto& root = slots_.front();
        root.size = miniStream_.size();
        miniStreamStart_ = nextSector_;
        root.start = miniStream_.empty() ? kEndOfChain : static_cast<std::uint32_t>(miniStreamStart_);
        nextSector_ += ceilDiv(miniStream_.size(), ss_);

        miniFatStart_ = nextSector_;
        miniFatSectors_ = ceilDiv(std::uint64_t{ miniFat_.size() } * 4, ss_);
        nextSector_ += miniFatSectors_;

        directoryStart_ = nextSector_;
        directorySectors_ = ceilDiv(std::uint64_t{ slots_.size() } * kDirEntrySize, ss_);
        nextSector_ += directorySectors_;

        std::uint64_t fat = 0;
        std::uint64_t difat = 0;
        while (true)
        {
            const std::uint64_t neededFat = ceilDiv(nextSector_ + fat + difat, perSector_);
            const std::uint64_t neededDifat = neededFat > kHeaderDifatEntries ? ceilDiv(neededFat - kHeaderDifatEntries, perSector_ - 1) : 0;
            if (neededFat == fat && neededDifat == difat)
            {
                break;
            }
            fat = neededFat;
            difat = neededDifat;
        }
        fatStart_ = nextSector_;
        fatSectors_ = fat;
        difatStart_ = fatStart_ + fatSectors_;
        difatSectors_ = difat;
        totalSectors_ = difatStart_ + difatSectors_;
    }

    auto emit() -> Result<void>
    {
        if (totalSectors_ > kMaxRegSect)
        {
            return fail(Errc::limit, "file is too large");
        }
        std::vector<std::uint32_t> fat(static_cast<std::size_t>(fatSectors_ * perSector_), kFreeSect);
        auto chain = [&fat](std::uint64_t start, std::uint64_t count) -> void {
            for (std::uint64_t i = 0; i < count; ++i)
            {
                fat[static_cast<std::size_t>(start + i)] = i + 1 == count ? kEndOfChain : static_cast<std::uint32_t>(start + i + 1);
            }
        };
        for (const auto index : bigStreams_)
        {
            chain(slots_[index].start, ceilDiv(slots_[index].size, ss_));
        }
        chain(miniStreamStart_, ceilDiv(miniStream_.size(), ss_));
        chain(miniFatStart_, miniFatSectors_);
        chain(directoryStart_, directorySectors_);
        for (std::uint64_t i = 0; i < fatSectors_; ++i)
        {
            fat[static_cast<std::size_t>(fatStart_ + i)] = kFatSect;
        }
        for (std::uint64_t i = 0; i < difatSectors_; ++i)
        {
            fat[static_cast<std::size_t>(difatStart_ + i)] = kDifSect;
        }

        if (auto r = emitHeader(); !r)
        {
            return r;
        }
        for (const auto index : bigStreams_)
        {
            if (auto r = emitStream(slots_[index]); !r)
            {
                return r;
            }
        }
        if (auto r = emitPadded(miniStream_); !r)
        {
            return r;
        }
        if (auto r = emitPadded(tableBytes(miniFat_, std::size_t{ miniFatSectors_ * perSector_ })); !r)
        {
            return r;
        }
        if (auto r = emitDirectory(); !r)
        {
            return r;
        }
        if (auto r = emitPadded(tableBytes(fat, fat.size())); !r)
        {
            return r;
        }
        return emitDifat();
    }

    [[nodiscard]] static auto streamBytes(const BuildNode& node) -> Result<std::vector<std::byte>>
    {
        if (const auto* bytes = std::get_if<std::vector<std::byte>>(&node.data))
        {
            return *bytes;
        }
        std::vector<std::byte> out(static_cast<std::size_t>(sizeOf(node.data)));
        auto got = readStreamData(node.data, 0, out);
        if (!got)
        {
            return std::unexpected(got.error());
        }
        if (*got != out.size())
        {
            return fail(Errc::io, std::format("stream '{}' ended early", toUtf8(node.name)));
        }
        return out;
    }

    static auto tableBytes(const std::vector<std::uint32_t>& table, std::size_t entries) -> std::vector<std::byte>
    {
        std::vector<std::byte> out(entries * 4);
        for (std::size_t i = 0; i < entries; ++i)
        {
            storeLE<std::uint32_t>(out, i * 4, i < table.size() ? table[i] : kFreeSect);
        }
        return out;
    }

    auto emitPadded(std::span<const std::byte> data) -> Result<void>
    {
        if (auto r = sink_.write(data); !r)
        {
            return r;
        }
        return emitPadding(data.size());
    }

    auto emitPadding(std::uint64_t written) -> Result<void>
    {
        const auto rem = static_cast<std::size_t>(written % ss_);
        if (rem == 0)
        {
            return {};
        }
        const std::vector<std::byte> zeros(ss_ - rem);
        return sink_.write(zeros);
    }

    auto emitStream(const Slot& slot) -> Result<void>
    {
        const auto& node = builder_.node(slot.node);
        if (const auto* bytes = std::get_if<std::vector<std::byte>>(&node.data))
        {
            return emitPadded(*bytes);
        }
        std::optional<StreamReader> reader;
        if (const auto* src = std::get_if<SourceStream>(&node.data))
        {
            auto opened = src->container->openStream(src->id);
            if (!opened)
            {
                return std::unexpected(opened.error());
            }
            reader = std::move(*opened);
        }
        std::vector<std::byte> buffer(kCopyChunk);
        std::uint64_t offset = 0;
        while (offset < slot.size)
        {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), slot.size - offset));
            auto got = reader ? reader->read(offset, std::span(buffer).first(n)) : readStreamData(node.data, offset, std::span(buffer).first(n));
            if (!got)
            {
                return std::unexpected(got.error());
            }
            if (*got != n)
            {
                return fail(Errc::io, std::format("stream '{}' ended early", toUtf8(node.name)));
            }
            if (auto r = sink_.write(std::span(buffer).first(n)); !r)
            {
                return r;
            }
            offset += n;
        }
        return emitPadding(slot.size);
    }

    auto emitHeader() -> Result<void>
    {
        std::vector<std::byte> h(ss_);
        constexpr std::array<std::uint8_t, 8> kSignature = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        std::ranges::transform(kSignature, h.begin(), [](std::uint8_t b) -> std::byte { return std::byte{ b }; });
        const bool v4 = builder_.version() == Version::v4;
        std::ranges::copy(builder_.headerClsid().bytes, h.begin() + 8);
        storeLE<std::uint16_t>(h, 24, 0x3E);
        storeLE<std::uint16_t>(h, 26, v4 ? 4 : 3);
        storeLE<std::uint16_t>(h, 28, 0xFFFE);
        storeLE<std::uint16_t>(h, 30, v4 ? 12 : 9);
        storeLE<std::uint16_t>(h, 32, 6);
        storeLE<std::uint32_t>(h, 40, v4 ? static_cast<std::uint32_t>(directorySectors_) : 0);
        storeLE<std::uint32_t>(h, 44, static_cast<std::uint32_t>(fatSectors_));
        storeLE<std::uint32_t>(h, 48, static_cast<std::uint32_t>(directoryStart_));
        storeLE<std::uint32_t>(h, 56, kMiniStreamCutoff);
        storeLE<std::uint32_t>(h, 60, miniFatSectors_ == 0 ? kEndOfChain : static_cast<std::uint32_t>(miniFatStart_));
        storeLE<std::uint32_t>(h, 64, static_cast<std::uint32_t>(miniFatSectors_));
        storeLE<std::uint32_t>(h, 68, difatSectors_ == 0 ? kEndOfChain : static_cast<std::uint32_t>(difatStart_));
        storeLE<std::uint32_t>(h, 72, static_cast<std::uint32_t>(difatSectors_));
        for (std::size_t i = 0; i < kHeaderDifatEntries; ++i)
        {
            storeLE<std::uint32_t>(h, 76 + i * 4, i < fatSectors_ ? static_cast<std::uint32_t>(fatStart_ + i) : kFreeSect);
        }
        return sink_.write(h);
    }

    auto emitDirectory() -> Result<void>
    {
        std::vector<std::byte> dir(static_cast<std::size_t>(directorySectors_ * ss_));
        for (std::size_t i = 0; i < dir.size() / kDirEntrySize; ++i)
        {
            const auto raw = std::span(dir).subspan(i * kDirEntrySize, kDirEntrySize);
            if (i >= slots_.size())
            {
                storeLE<std::uint32_t>(raw, 68, kNoStream);
                storeLE<std::uint32_t>(raw, 72, kNoStream);
                storeLE<std::uint32_t>(raw, 76, kNoStream);
                continue;
            }
            const auto& slot = slots_[i];
            const auto& node = builder_.node(slot.node);
            const std::u16string_view name = i == 0 ? std::u16string_view(u"Root Entry") : std::u16string_view(node.name);
            for (std::size_t k = 0; k < name.size(); ++k)
            {
                storeLE<std::uint16_t>(raw, k * 2, static_cast<std::uint16_t>(name[k]));
            }
            storeLE<std::uint16_t>(raw, 64, static_cast<std::uint16_t>((name.size() + 1) * 2));
            raw[66] = std::byte{ static_cast<std::uint8_t>(i == 0 ? EntryType::root : node.type) };
            raw[67] = std::byte{ static_cast<std::uint8_t>(slot.red ? 0 : 1) };
            storeLE<std::uint32_t>(raw, 68, slot.left);
            storeLE<std::uint32_t>(raw, 72, slot.right);
            storeLE<std::uint32_t>(raw, 76, slot.child);
            std::ranges::copy(node.clsid.bytes, raw.begin() + 80);
            storeLE<std::uint32_t>(raw, 96, node.stateBits);
            storeLE<std::uint64_t>(raw, 100, node.creationTime);
            storeLE<std::uint64_t>(raw, 108, node.modifiedTime);
            storeLE<std::uint32_t>(raw, 116, slot.start);
            storeLE<std::uint64_t>(raw, 120, slot.size);
        }
        return sink_.write(dir);
    }

    auto emitDifat() -> Result<void>
    {
        std::uint64_t next = kHeaderDifatEntries;
        for (std::uint64_t s = 0; s < difatSectors_; ++s)
        {
            std::vector<std::byte> sector(ss_);
            for (std::uint32_t k = 0; k + 1 < perSector_; ++k, ++next)
            {
                storeLE<std::uint32_t>(sector, std::size_t{ k } * 4, next < fatSectors_ ? static_cast<std::uint32_t>(fatStart_ + next) : kFreeSect);
            }
            const auto link = s + 1 == difatSectors_ ? kEndOfChain : static_cast<std::uint32_t>(difatStart_ + s + 1);
            storeLE<std::uint32_t>(sector, std::size_t{ perSector_ - 1 } * 4, link);
            if (auto r = sink_.write(sector); !r)
            {
                return r;
            }
        }
        return {};
    }

    const Builder& builder_;
    ByteSink& sink_;
    std::uint32_t ss_;
    std::uint32_t perSector_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> bigStreams_;
    std::vector<std::byte> miniStream_;
    std::vector<std::uint32_t> miniFat_;
    std::uint64_t nextSector_ = 0;
    std::uint64_t miniStreamStart_ = 0;
    std::uint64_t miniFatStart_ = 0;
    std::uint64_t miniFatSectors_ = 0;
    std::uint64_t directoryStart_ = 0;
    std::uint64_t directorySectors_ = 0;
    std::uint64_t fatStart_ = 0;
    std::uint64_t fatSectors_ = 0;
    std::uint64_t difatStart_ = 0;
    std::uint64_t difatSectors_ = 0;
    std::uint64_t totalSectors_ = 0;
};

void copyChildren(const Container& source, EntryId from, Builder& builder, NodeId to, Result<void>& status)
{
    for (const auto childId : source.entry(from).children)
    {
        if (!status)
        {
            return;
        }
        const auto& child = source.entry(childId);
        auto added = child.isStream() ? builder.addStream(to, child.name, SourceStream{ &source, childId }) : builder.addStorage(to, child.name, child.clsid);
        if (!added)
        {
            status = std::unexpected(added.error());
            return;
        }
        auto& node = builder.node(*added);
        node.clsid = child.clsid;
        node.stateBits = child.stateBits;
        node.creationTime = child.creationTime;
        node.modifiedTime = child.modifiedTime;
        if (child.isStorage())
        {
            copyChildren(source, childId, builder, *added, status);
        }
    }
}

}

namespace detail
{

auto layoutSiblings(std::uint32_t count) -> TreeLayout
{
    TreeLayout layout;
    layout.left.assign(count, kNoStream);
    layout.right.assign(count, kNoStream);
    layout.red.assign(count, false);
    if (count == 0)
    {
        return layout;
    }
    std::vector<std::uint32_t> depth(count, 0);
    std::uint32_t maxDepth = 0;
    struct Range
    {
        std::uint32_t lo;
        std::uint32_t hi;
        std::uint32_t depth;
        std::uint32_t parent;
        bool isLeft;
    };
    std::vector<Range> work = { { 0, count, 0, kNoStream, false } };
    while (!work.empty())
    {
        const auto r = work.back();
        work.pop_back();
        if (r.lo >= r.hi)
        {
            continue;
        }
        const std::uint32_t mid = r.lo + (r.hi - r.lo) / 2;
        depth[mid] = r.depth;
        maxDepth = std::max(maxDepth, r.depth);
        if (r.parent == kNoStream)
        {
            layout.root = mid;
        }
        else if (r.isLeft)
        {
            layout.left[r.parent] = mid;
        }
        else
        {
            layout.right[r.parent] = mid;
        }
        work.push_back({ r.lo, mid, r.depth + 1, mid, true });
        work.push_back({ mid + 1, r.hi, r.depth + 1, mid, false });
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        layout.red[i] = maxDepth > 0 && depth[i] == maxDepth;
    }
    return layout;
}

}

Builder::Builder(Version version) :
    version_(version)
{
    BuildNode root;
    root.name = u"Root Entry";
    nodes_.push_back(std::move(root));
}

auto Builder::fromContainer(const Container& source) -> Result<Builder>
{
    Builder builder(source.header().version);
    builder.setHeaderClsid(source.header().clsid);
    auto& rootNode = builder.node(Builder::root());
    rootNode.clsid = source.root().clsid;
    rootNode.stateBits = source.root().stateBits;
    rootNode.creationTime = source.root().creationTime;
    rootNode.modifiedTime = source.root().modifiedTime;
    Result<void> status;
    copyChildren(source, 0, builder, Builder::root(), status);
    if (!status)
    {
        return std::unexpected(status.error());
    }
    return builder;
}

auto Builder::addNode(NodeId parent, BuildNode node) -> Result<NodeId>
{
    if (parent >= nodes_.size() || nodes_[parent].type != EntryType::storage)
    {
        return fail(Errc::invalid_argument, "parent is not a storage");
    }
    if (auto r = validateName(node.name); !r)
    {
        return std::unexpected(r.error());
    }
    std::u16string key{ static_cast<char16_t>(parent >> 16U), static_cast<char16_t>(parent & 0xFFFFU) };
    std::ranges::transform(node.name, std::back_inserter(key), upperCase);
    if (!names_.insert(std::move(key)).second)
    {
        return fail(Errc::invalid_argument, std::format("duplicate entry name '{}'", toUtf8(node.name)));
    }
    const auto id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(std::move(node));
    nodes_[parent].children.push_back(id);
    return id;
}

auto Builder::addStorage(NodeId parent, std::u16string name, Clsid clsid) -> Result<NodeId>
{
    BuildNode node;
    node.name = std::move(name);
    node.clsid = clsid;
    return addNode(parent, std::move(node));
}

auto Builder::addStream(NodeId parent, std::u16string name, StreamData data) -> Result<NodeId>
{
    if (const auto* src = std::get_if<SourceStream>(&data); src != nullptr && (src->container == nullptr || src->id >= src->container->entries().size() || !src->container->entry(src->id).isStream()))
    {
        return fail(Errc::invalid_argument, "source stream does not refer to a stream");
    }
    BuildNode node;
    node.name = std::move(name);
    node.type = EntryType::stream;
    node.data = std::move(data);
    return addNode(parent, std::move(node));
}

auto Builder::streamSize(NodeId id) const -> std::uint64_t
{
    return sizeOf(nodes_.at(id).data);
}

auto sizeOf(const StreamData& data) -> std::uint64_t
{
    if (const auto* bytes = std::get_if<std::vector<std::byte>>(&data))
    {
        return bytes->size();
    }
    if (const auto* shared = std::get_if<SharedSource>(&data))
    {
        return shared->source ? shared->source->size() : 0;
    }
    const auto& src = std::get<SourceStream>(data);
    return src.container->entry(src.id).size;
}

auto readStreamData(const StreamData& data, std::uint64_t offset, std::span<std::byte> out) -> Result<std::size_t>
{
    if (const auto* bytes = std::get_if<std::vector<std::byte>>(&data))
    {
        if (offset >= bytes->size())
        {
            return 0;
        }
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), bytes->size() - offset));
        std::copy_n(bytes->begin() + static_cast<std::ptrdiff_t>(offset), n, out.begin());
        return n;
    }
    if (const auto* shared = std::get_if<SharedSource>(&data))
    {
        if (!shared->source)
        {
            return 0;
        }
        return shared->source->read(offset, out);
    }
    const auto& src = std::get<SourceStream>(data);
    auto reader = src.container->openStream(src.id);
    if (!reader)
    {
        return std::unexpected(reader.error());
    }
    return reader->read(offset, out);
}

auto write(const Builder& builder, ByteSink& sink) -> Result<void>
{
    return Writer(builder, sink).run();
}

auto writeFile(const Builder& builder, const std::filesystem::path& path) -> Result<void>
{
    return writeFileAtomic(path, [&builder](ByteSink& sink) -> Result<void> { return write(builder, sink); });
}

}
