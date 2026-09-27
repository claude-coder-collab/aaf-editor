#include "fixtures.hpp"

#include <aaf/cfb/container.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace aaf::cfb;
using namespace aaf::test;

namespace
{

auto sampleFile(Version version) -> std::vector<std::byte>
{
    Builder b(version);
    const auto s = b.addStorage(Builder::root(), u"storage").value();
    (void) b.addStream(s, u"big", patternBytes(10000, 1)).value();
    (void) b.addStream(s, u"small", patternBytes(300, 2)).value();
    (void) b.addStream(Builder::root(), u"other", patternBytes(5000, 3)).value();
    return writeToMemory(b).value();
}

auto u32At(const std::vector<std::byte>& d, std::size_t off) -> std::uint32_t
{
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

void setU32(std::vector<std::byte>& d, std::size_t off, std::uint32_t v)
{
    std::memcpy(d.data() + off, &v, 4);
}

void touchEverything(const Container& c)
{
    for (const auto& e : c.entries())
    {
        if (e.isStream())
        {
            if (auto r = c.openStream(e.id))
            {
                (void) r->readAll(1 << 24);
            }
        }
    }
}

}

TEST_CASE("Malformed headers are rejected", "[cfb][corruption]")
{
    auto good = sampleFile(Version::v3);
    REQUIRE(openMemory(good));

    CHECK_FALSE(openMemory({}));
    CHECK_FALSE(openMemory(std::vector<std::byte>(good.begin(), good.begin() + 100)));

    auto badSig = good;
    badSig[0] = std::byte{ 0 };
    CHECK(openMemory(badSig).error().code == aaf::Errc::format);

    auto badVersion = good;
    badVersion[26] = std::byte{ 4 };
    CHECK(openMemory(badVersion).error().code == aaf::Errc::unsupported);

    auto hugeFat = good;
    setU32(hugeFat, 44, 0x7FFFFFFF);
    CHECK_FALSE(openMemory(hugeFat));

    auto truncated = good;
    truncated.resize(good.size() / 2);
    CHECK_FALSE(openMemory(truncated));
}

TEST_CASE("Cycles in sector chains are detected", "[cfb][corruption]")
{
    auto data = sampleFile(Version::v3);
    const std::size_t fatOffset = (std::size_t{ u32At(data, 76) } + 1) * 512;
    setU32(data, fatOffset, 0);
    auto c = openMemory(data);
    REQUIRE(c);
    const auto big = c->findPath("other");
    REQUIRE(big);
    const auto reader = c->openStream(*big);
    REQUIRE_FALSE(reader);
    CHECK(reader.error().message.find("cycle") != std::string::npos);
}

TEST_CASE("Cycles in the directory tree are detected", "[cfb][corruption]")
{
    auto data = sampleFile(Version::v4);
    const std::size_t dirOffset = (std::size_t{ u32At(data, 48) } + 1) * 4096;
    const auto rootChild = u32At(data, dirOffset + 76);
    setU32(data, dirOffset + std::size_t{ rootChild } * 128 + 68, rootChild);
    const auto c = openMemory(data);
    REQUIRE_FALSE(c);
    CHECK(c.error().message.find("more than once") != std::string::npos);

    auto rootLoop = sampleFile(Version::v4);
    setU32(rootLoop, dirOffset + 76, 0);
    CHECK_FALSE(openMemory(rootLoop));
}

TEST_CASE("Random corruption never crashes the reader", "[cfb][corruption]")
{
    std::vector<std::vector<std::byte>> seeds = { sampleFile(Version::v3), sampleFile(Version::v4) };
    for (const auto& path : aafFilesIn("aafsdk"))
    {
        seeds.push_back(readFile(path).value());
        break;
    }
    std::uint32_t state = 12345;
    auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return state;
    };
    for (const auto& seed : seeds)
    {
        for (int iteration = 0; iteration < 300; ++iteration)
        {
            auto data = seed;
            const int flips = 1 + static_cast<int>(next() % 8);
            for (int k = 0; k < flips; ++k)
            {
                const auto region = next() % 4;
                const std::size_t limit = region == 0 ? std::min<std::size_t>(512, data.size()) : data.size();
                data[next() % limit] = static_cast<std::byte>(next());
            }
            if (next() % 10 == 0)
            {
                data.resize(next() % data.size());
            }
            if (auto c = openMemory(std::move(data)))
            {
                touchEverything(*c);
            }
        }
    }
    SUCCEED();
}
