#include "endian.hpp"

#include <aaf/cfb/container.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace aaf::cfb
{

namespace
{

using detail::loadLE;

constexpr std::array<std::byte, 8> kSignature = {
    std::byte{ 0xD0 },
    std::byte{ 0xCF },
    std::byte{ 0x11 },
    std::byte{ 0xE0 },
    std::byte{ 0xA1 },
    std::byte{ 0xB1 },
    std::byte{ 0x1A },
    std::byte{ 0xE1 }
};
constexpr std::size_t kHeaderSize = 512;

auto readExact(const ByteSource& source, std::uint64_t offset, std::span<std::byte> out) -> Result<void>
{
    auto n = source.read(offset, out);
    if (!n)
    {
        return std::unexpected(n.error());
    }
    if (*n != out.size())
    {
        return fail(Errc::format, "unexpected end of file", offset + *n);
    }
    return {};
}

auto parseEntryType(std::uint8_t raw) -> std::optional<EntryType>
{
    switch (raw)
    {
        case 0:
            return EntryType::empty;
        case 1:
            return EntryType::storage;
        case 2:
            return EntryType::stream;
        case 5:
            return EntryType::root;
        default:
            return std::nullopt;
    }
}

struct RawLinks
{
    std::uint32_t left = kNoStream;
    std::uint32_t right = kNoStream;
    std::uint32_t child = kNoStream;
};

}

auto followChain(std::span<const std::uint32_t> table, std::uint32_t start) -> Result<std::vector<std::uint32_t>>
{
    std::vector<std::uint32_t> chain;
    std::uint32_t sector = start;
    while (sector != kEndOfChain)
    {
        if (sector > kMaxRegSect || sector >= table.size())
        {
            return fail(Errc::format, std::format("sector chain starting at {} references invalid sector {:#x}", start, sector));
        }
        chain.push_back(sector);
        if (chain.size() > table.size())
        {
            return fail(Errc::format, std::format("sector chain starting at {} contains a cycle", start));
        }
        sector = table[sector];
    }
    return chain;
}

StreamReader::StreamReader(const ByteSource* source, std::uint64_t size, std::uint32_t unitSize, std::vector<std::uint64_t> unitOffsets) :
    source_(source),
    size_(size),
    unitSize_(unitSize),
    unitOffsets_(std::move(unitOffsets))
{
}

auto StreamReader::read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t>
{
    if (offset >= size_)
    {
        return 0;
    }
    const auto total = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size_ - offset));
    std::size_t done = 0;
    while (done < total)
    {
        const std::uint64_t pos = offset + done;
        const auto unit = static_cast<std::size_t>(pos / unitSize_);
        const auto within = static_cast<std::size_t>(pos % unitSize_);
        const std::size_t n = std::min<std::size_t>(unitSize_ - within, total - done);
        const std::uint64_t absolute = unitOffsets_[unit] + within;
        auto got = source_->read(absolute, out.subspan(done, n));
        if (!got)
        {
            return std::unexpected(got.error());
        }
        if (*got < n)
        {
            if (unitOffsets_[unit] >= source_->size())
            {
                return fail(Errc::format, "stream data lies beyond end of file", absolute);
            }
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(done + *got), out.begin() + static_cast<std::ptrdiff_t>(done + n), std::byte{ 0 });
        }
        done += n;
    }
    return total;
}

auto StreamReader::readAll(std::uint64_t maxSize) const -> Result<std::vector<std::byte>>
{
    if (size_ > maxSize)
    {
        return fail(Errc::limit, std::format("stream of {} bytes exceeds limit of {} bytes", size_, maxSize));
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size_));
    auto n = read(0, data);
    if (!n)
    {
        return std::unexpected(n.error());
    }
    return data;
}

auto Container::openFile(const std::filesystem::path& path) -> Result<Container>
{
    auto source = FileSource::open(path);
    if (!source)
    {
        return std::unexpected(source.error());
    }
    return open(std::move(*source));
}

auto Container::open(std::unique_ptr<ByteSource> source) -> Result<Container>
{
    if (!source)
    {
        return fail(Errc::invalid_argument, "null byte source");
    }
    Container c;
    c.source_ = std::move(source);

    std::array<std::byte, kHeaderSize> header{};
    if (c.source_->size() < kHeaderSize)
    {
        return fail(Errc::format, "file is too small to be a compound file");
    }
    if (auto r = readExact(*c.source_, 0, header); !r)
    {
        return std::unexpected(r.error());
    }
    if (!std::equal(kSignature.begin(), kSignature.end(), header.begin()))
    {
        return fail(Errc::format, "not a compound file (bad signature)", 0);
    }
    if (loadLE<std::uint16_t>(header, 28) != 0xFFFE)
    {
        return fail(Errc::format, "invalid byte order mark", 28);
    }
    const auto major = loadLE<std::uint16_t>(header, 26);
    const auto sectorShift = loadLE<std::uint16_t>(header, 30);
    if (major == 3 && sectorShift == 9)
    {
        c.header_.version = Version::v3;
    }
    else if (major == 4 && sectorShift == 12)
    {
        c.header_.version = Version::v4;
    }
    else
    {
        return fail(Errc::unsupported, std::format("unsupported version {} with sector shift {}", major, sectorShift), 26);
    }
    if (loadLE<std::uint16_t>(header, 32) != 6)
    {
        return fail(Errc::format, "invalid mini sector shift", 32);
    }
    if (loadLE<std::uint32_t>(header, 56) != kMiniStreamCutoff)
    {
        return fail(Errc::format, "invalid mini stream cutoff size", 56);
    }
    c.header_.minorVersion = loadLE<std::uint16_t>(header, 24);
    std::copy_n(header.begin() + 8, 16, c.header_.clsid.bytes.begin());
    c.header_.sectorSize = sectorSize(c.header_.version);
    c.header_.directorySectors = loadLE<std::uint32_t>(header, 40);
    c.header_.fatSectors = loadLE<std::uint32_t>(header, 44);
    c.header_.miniFatSectors = loadLE<std::uint32_t>(header, 64);
    c.header_.difatSectors = loadLE<std::uint32_t>(header, 72);

    if (auto r = c.loadFat(header); !r)
    {
        return std::unexpected(r.error());
    }
    if (auto r = c.loadDirectory(loadLE<std::uint32_t>(header, 48)); !r)
    {
        return std::unexpected(r.error());
    }
    if (auto r = c.loadMiniStream(loadLE<std::uint32_t>(header, 60)); !r)
    {
        return std::unexpected(r.error());
    }
    return c;
}

auto Container::sectorOffset(std::uint32_t sector) const noexcept -> std::uint64_t
{
    return (std::uint64_t{ sector } + 1) * header_.sectorSize;
}

auto Container::readSectors(std::span<const std::uint32_t> chain, std::uint64_t maxBytes) const -> Result<std::vector<std::byte>>
{
    const std::uint64_t total = std::min<std::uint64_t>(std::uint64_t{ chain.size() } * header_.sectorSize, maxBytes);
    if (total > source_->size())
    {
        return fail(Errc::format, "sector chain is longer than the file");
    }
    std::vector<std::byte> data(static_cast<std::size_t>(total));
    std::size_t done = 0;
    for (const auto sector : chain)
    {
        if (done >= data.size())
        {
            break;
        }
        const auto offset = sectorOffset(sector);
        if (offset >= source_->size())
        {
            return fail(Errc::format, std::format("sector {} lies beyond end of file", sector), offset);
        }
        const std::size_t n = std::min<std::size_t>(header_.sectorSize, data.size() - done);
        auto got = source_->read(offset, std::span(data).subspan(done, n));
        if (!got)
        {
            return std::unexpected(got.error());
        }
        done += n;
    }
    return data;
}

auto Container::loadFat(std::span<const std::byte> header) -> Result<void>
{
    const std::uint32_t ss = header_.sectorSize;
    const std::uint32_t perSector = ss / 4;
    const std::uint64_t fileSectors = (source_->size() + ss - 1) / ss;
    if (header_.fatSectors > fileSectors || header_.difatSectors > fileSectors)
    {
        return fail(Errc::format, "FAT or DIFAT sector count exceeds file size", 44);
    }

    std::vector<std::uint32_t> fatSectors;
    fatSectors.reserve(header_.fatSectors);
    for (std::size_t i = 0; i < kHeaderDifatEntries && fatSectors.size() < header_.fatSectors; ++i)
    {
        fatSectors.push_back(loadLE<std::uint32_t>(header, 76 + i * 4));
    }

    auto difat = loadLE<std::uint32_t>(header, 68);
    std::vector<std::byte> buffer(ss);
    for (std::uint32_t i = 0; i < header_.difatSectors && fatSectors.size() < header_.fatSectors; ++i)
    {
        if (difat > kMaxRegSect)
        {
            return fail(Errc::format, "DIFAT chain ends early", 68);
        }
        if (auto r = readExact(*source_, sectorOffset(difat), buffer); !r)
        {
            return std::unexpected(r.error());
        }
        for (std::uint32_t k = 0; k + 1 < perSector && fatSectors.size() < header_.fatSectors; ++k)
        {
            fatSectors.push_back(loadLE<std::uint32_t>(buffer, std::size_t{ k } * 4));
        }
        difat = loadLE<std::uint32_t>(buffer, std::size_t{ perSector - 1 } * 4);
    }
    if (fatSectors.size() < header_.fatSectors)
    {
        return fail(Errc::format, std::format("DIFAT lists {} FAT sectors, header declares {}", fatSectors.size(), header_.fatSectors));
    }

    fat_.reserve(std::size_t{ header_.fatSectors } * perSector);
    for (const auto sector : fatSectors)
    {
        if (sector > kMaxRegSect)
        {
            return fail(Errc::format, std::format("invalid FAT sector location {:#x}", sector));
        }
        if (auto r = readExact(*source_, sectorOffset(sector), buffer); !r)
        {
            return std::unexpected(r.error());
        }
        for (std::uint32_t k = 0; k < perSector; ++k)
        {
            fat_.push_back(loadLE<std::uint32_t>(buffer, std::size_t{ k } * 4));
        }
    }
    return {};
}

auto Container::loadDirectory(std::uint32_t firstSector) -> Result<void>
{
    auto chain = followChain(fat_, firstSector);
    if (!chain)
    {
        return std::unexpected(chain.error());
    }
    if (chain->empty())
    {
        return fail(Errc::format, "directory is empty", 48);
    }
    auto data = readSectors(*chain, UINT64_MAX);
    if (!data)
    {
        return std::unexpected(data.error());
    }

    const std::size_t count = data->size() / kDirEntrySize;
    entries_.resize(count);
    std::vector<RawLinks> links(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto raw = std::span<const std::byte>(*data).subspan(i * kDirEntrySize, kDirEntrySize);
        auto& e = entries_[i];
        e.id = static_cast<EntryId>(i);
        const auto type = parseEntryType(std::to_integer<std::uint8_t>(raw[66]));
        if (!type)
        {
            return fail(Errc::format, std::format("directory entry {} has invalid type {}", i, std::to_integer<int>(raw[66])));
        }
        e.type = *type;
        const auto nameBytes = loadLE<std::uint16_t>(raw, 64);
        if (e.type != EntryType::empty)
        {
            if (nameBytes > 64 || nameBytes % 2 != 0)
            {
                return fail(Errc::format, std::format("directory entry {} has invalid name length {}", i, nameBytes));
            }
            for (std::size_t k = 0; k < nameBytes / 2U; ++k)
            {
                const auto c = static_cast<char16_t>(loadLE<std::uint16_t>(raw, k * 2));
                if (c == 0)
                {
                    break;
                }
                e.name.push_back(c);
            }
        }
        std::copy_n(raw.begin() + 80, 16, e.clsid.bytes.begin());
        e.stateBits = loadLE<std::uint32_t>(raw, 96);
        e.creationTime = loadLE<std::uint64_t>(raw, 100);
        e.modifiedTime = loadLE<std::uint64_t>(raw, 108);
        e.startSector = loadLE<std::uint32_t>(raw, 116);
        e.size = loadLE<std::uint64_t>(raw, 120);
        if (header_.version == Version::v3)
        {
            e.size &= 0xFFFFFFFFU;
        }
        links[i] = { loadLE<std::uint32_t>(raw, 68), loadLE<std::uint32_t>(raw, 72), loadLE<std::uint32_t>(raw, 76) };
    }
    if (entries_[0].type != EntryType::root)
    {
        return fail(Errc::format, "first directory entry is not the root entry");
    }

    std::vector<bool> visited(count, false);
    visited[0] = true;
    std::vector<EntryId> storages = { 0 };
    while (!storages.empty())
    {
        const EntryId parent = storages.back();
        storages.pop_back();
        std::vector<EntryId> stack;
        std::uint32_t node = links[parent].child;
        while (node != kNoStream || !stack.empty())
        {
            while (node != kNoStream)
            {
                if (node >= count || entries_[node].type == EntryType::empty || entries_[node].type == EntryType::root)
                {
                    return fail(Errc::format, std::format("directory entry {} links to invalid entry {}", parent, node));
                }
                if (visited[node])
                {
                    return fail(Errc::format, std::format("directory entry {} is linked more than once", node));
                }
                visited[node] = true;
                stack.push_back(node);
                node = links[node].left;
            }
            const EntryId current = stack.back();
            stack.pop_back();
            entries_[current].parent = parent;
            entries_[parent].children.push_back(current);
            if (entries_[current].isStorage())
            {
                storages.push_back(current);
            }
            node = links[current].right;
        }
    }
    for (std::size_t i = 1; i < count; ++i)
    {
        if (!visited[i])
        {
            entries_[i] = DirEntry{};
            entries_[i].id = static_cast<EntryId>(i);
        }
    }
    return {};
}

auto Container::loadMiniStream(std::uint32_t firstMiniFatSector) -> Result<void>
{
    auto miniFatChain = followChain(fat_, firstMiniFatSector);
    if (!miniFatChain)
    {
        return std::unexpected(miniFatChain.error());
    }
    auto miniFatData = readSectors(*miniFatChain, UINT64_MAX);
    if (!miniFatData)
    {
        return std::unexpected(miniFatData.error());
    }
    miniFat_.resize(miniFatData->size() / 4);
    for (std::size_t i = 0; i < miniFat_.size(); ++i)
    {
        miniFat_[i] = loadLE<std::uint32_t>(*miniFatData, i * 4);
    }

    const auto& root = entries_.front();
    if (root.size == 0)
    {
        return {};
    }
    auto chain = followChain(fat_, root.startSector);
    if (!chain)
    {
        return std::unexpected(chain.error());
    }
    if (std::uint64_t{ chain->size() } * header_.sectorSize < root.size)
    {
        return fail(Errc::format, "mini stream is shorter than its declared size");
    }
    miniStreamChain_ = std::move(*chain);
    return {};
}

auto Container::find(EntryId parent, std::u16string_view name) const -> std::optional<EntryId>
{
    const auto& children = entries_.at(parent).children;
    const auto it = std::ranges::find_if(children, [&](EntryId id) -> bool { return compareNames(entries_[id].name, name) == 0; });
    return it == children.end() ? std::nullopt : std::optional(*it);
}

auto Container::findPath(std::string_view path) const -> std::optional<EntryId>
{
    EntryId current = 0;
    while (!path.empty())
    {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (part.empty())
        {
            continue;
        }
        auto name = toUtf16(part);
        if (!name)
        {
            return std::nullopt;
        }
        auto next = find(current, *name);
        if (!next)
        {
            return std::nullopt;
        }
        current = *next;
    }
    return current;
}

auto Container::openStream(EntryId id) const -> Result<StreamReader>
{
    if (id >= entries_.size() || !entries_[id].isStream())
    {
        return fail(Errc::invalid_argument, std::format("entry {} is not a stream", id));
    }
    const auto& e = entries_[id];
    if (e.size == 0)
    {
        return StreamReader(source_.get(), 0, header_.sectorSize, {});
    }

    const bool mini = e.size < kMiniStreamCutoff;
    const std::uint32_t unit = mini ? kMiniSectorSize : header_.sectorSize;
    auto chain = followChain(mini ? miniFat_ : fat_, e.startSector);
    if (!chain)
    {
        return std::unexpected(chain.error());
    }
    const auto needed = static_cast<std::size_t>((e.size + unit - 1) / unit);
    if (chain->size() < needed)
    {
        return fail(Errc::format, std::format("stream '{}' is shorter than its declared size", toUtf8(e.name)));
    }

    std::vector<std::uint64_t> offsets;
    offsets.reserve(needed);
    for (std::size_t i = 0; i < needed; ++i)
    {
        const std::uint32_t sector = (*chain)[i];
        if (!mini)
        {
            offsets.push_back(sectorOffset(sector));
            continue;
        }
        const std::uint64_t miniOffset = std::uint64_t{ sector } * kMiniSectorSize;
        const auto index = static_cast<std::size_t>(miniOffset / header_.sectorSize);
        if (miniOffset + kMiniSectorSize > root().size || index >= miniStreamChain_.size())
        {
            return fail(Errc::format, std::format("stream '{}' references mini sector {} outside the mini stream", toUtf8(e.name), sector));
        }
        offsets.push_back(sectorOffset(miniStreamChain_[index]) + miniOffset % header_.sectorSize);
    }
    return StreamReader(source_.get(), e.size, unit, std::move(offsets));
}

}
