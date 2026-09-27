#include "fixtures.hpp"

#include <algorithm>
#include <format>

namespace aaf::test
{

namespace
{

auto diffEntry(const cfb::Container& a, cfb::EntryId ia, const cfb::Container& b, cfb::EntryId ib, const std::string& path) -> std::string
{
    const auto& ea = a.entry(ia);
    const auto& eb = b.entry(ib);
    if (ea.type != eb.type && !(ia == 0 && ib == 0))
    {
        return std::format("{}: type differs", path);
    }
    if (ea.clsid != eb.clsid || ea.stateBits != eb.stateBits || ea.creationTime != eb.creationTime || ea.modifiedTime != eb.modifiedTime)
    {
        return std::format("{}: metadata differs", path);
    }
    if (ea.isStream())
    {
        auto ra = a.openStream(ia);
        auto rb = b.openStream(ib);
        if (!ra || !rb)
        {
            return std::format("{}: cannot open stream", path);
        }
        auto da = ra->readAll();
        auto db = rb->readAll();
        if (!da || !db)
        {
            return std::format("{}: cannot read stream", path);
        }
        return *da == *db ? std::string{} : std::format("{}: stream contents differ", path);
    }
    if (ea.children.size() != eb.children.size())
    {
        return std::format("{}: child count {} vs {}", path, ea.children.size(), eb.children.size());
    }
    for (std::size_t i = 0; i < ea.children.size(); ++i)
    {
        const auto& ca = a.entry(ea.children[i]);
        const auto& cb = b.entry(eb.children[i]);
        const auto childPath = path + "/" + cfb::toUtf8(ca.name);
        if (ca.name != cb.name)
        {
            return std::format("{}: name differs from '{}'", childPath, cfb::toUtf8(cb.name));
        }
        if (auto d = diffEntry(a, ea.children[i], b, eb.children[i], childPath); !d.empty())
        {
            return d;
        }
    }
    return {};
}

}

auto fixturesDir() -> std::filesystem::path
{
    return AAF_FIXTURES_DIR;
}

auto aafFilesIn(const std::filesystem::path& sub) -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> files;
    const auto dir = fixturesDir() / sub;
    if (!std::filesystem::is_directory(dir))
    {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".aaf")
        {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

auto bytesOf(std::string_view text) -> std::vector<std::byte>
{
    std::vector<std::byte> out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

auto patternBytes(std::size_t size, std::uint32_t seed) -> std::vector<std::byte>
{
    std::vector<std::byte> out(size);
    std::uint32_t state = seed * 2654435761U + 1;
    for (auto& b : out)
    {
        state = state * 1664525U + 1013904223U;
        b = static_cast<std::byte>(state >> 24);
    }
    return out;
}

auto openMemory(std::vector<std::byte> data) -> Result<cfb::Container>
{
    return cfb::Container::open(std::make_unique<cfb::MemorySource>(std::move(data)));
}

auto writeToMemory(const cfb::Builder& builder) -> Result<std::vector<std::byte>>
{
    cfb::MemorySink sink;
    if (auto r = cfb::write(builder, sink); !r)
    {
        return std::unexpected(r.error());
    }
    return sink.take();
}

auto diffTrees(const cfb::Container& a, const cfb::Container& b) -> std::string
{
    return diffEntry(a, 0, b, 0, "");
}

}
