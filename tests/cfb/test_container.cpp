#include "fixtures.hpp"

#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>

#include <catch2/catch_test_macros.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <compare>

#include <algorithm>
#include <format>
#include <random>

using namespace aaf::cfb;
using namespace aaf::test;

namespace
{

auto streamContents(const Container& c, std::string_view path) -> std::vector<std::byte>
{
    const auto id = c.findPath(path);
    REQUIRE(id);
    auto reader = c.openStream(*id);
    REQUIRE(reader);
    auto data = reader->readAll();
    REQUIRE(data);
    return *data;
}

}

TEST_CASE("Streams of every size class round-trip", "[cfb][container]")
{
    const auto version = GENERATE(Version::v3, Version::v4);
    const std::vector<std::size_t> sizes = { 0, 1, 63, 64, 65, 4095, 4096, 4097, 511, 512, 513, 70000, 1 << 20 };

    Builder b(version);
    const auto dir = b.addStorage(Builder::root(), u"dir", parseClsid("{0d010201-0200-0000-060e-2b3403020101}").value());
    REQUIRE(dir);
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        REQUIRE(b.addStream(*dir, toUtf16(std::format("s{}", i)).value(), patternBytes(sizes[i], static_cast<std::uint32_t>(i))));
    }
    b.node(*dir).stateBits = 7;
    b.node(*dir).creationTime = 0x01D0000000000001;

    auto bytes = writeToMemory(b);
    REQUIRE(bytes);
    CHECK(bytes->size() % sectorSize(version) == 0);
    auto c = openMemory(std::move(*bytes));
    REQUIRE(c);
    CHECK(c->header().version == version);

    const auto dirId = c->findPath("dir");
    REQUIRE(dirId);
    CHECK(c->entry(*dirId).stateBits == 7);
    CHECK(c->entry(*dirId).creationTime == 0x01D0000000000001);
    CHECK(toString(c->entry(*dirId).clsid) == "{0d010201-0200-0000-060e-2b3403020101}");
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        INFO("size " << sizes[i]);
        CHECK(streamContents(*c, std::format("dir/s{}", i)) == patternBytes(sizes[i], static_cast<std::uint32_t>(i)));
    }
}

TEST_CASE("Random-access reads cross sector boundaries", "[cfb][container]")
{
    const auto data = patternBytes(20000, 3);
    const auto small = patternBytes(3000, 4);
    Builder b(Version::v3);
    REQUIRE(b.addStream(Builder::root(), u"big", data));
    REQUIRE(b.addStream(Builder::root(), u"small", small));
    auto c = openMemory(writeToMemory(b).value());
    REQUIRE(c);

    for (const auto& [name, expected] : { std::pair{ "big", data }, std::pair{ "small", small } })
    {
        auto reader = c->openStream(c->findPath(name).value());
        REQUIRE(reader);
        std::vector<std::byte> buf(700);
        for (std::uint64_t offset : { 0ULL, 60ULL, 511ULL, 1000ULL, 2999ULL })
        {
            const auto n = reader->read(offset, buf);
            REQUIRE(n);
            const auto expectedN = std::min<std::size_t>(buf.size(), expected.size() - offset);
            REQUIRE(*n == expectedN);
            CHECK(std::equal(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n), expected.begin() + static_cast<std::ptrdiff_t>(offset)));
        }
        CHECK(reader->read(expected.size(), buf).value() == 0);
        CHECK_FALSE(reader->readAll(10));
    }
}

TEST_CASE("Deep and wide trees round-trip in sorted order", "[cfb][container]")
{
    Builder b(Version::v4);
    NodeId parent = Builder::root();
    for (int depth = 0; depth < 20; ++depth)
    {
        parent = b.addStorage(parent, toUtf16(std::format("level{}", depth)).value()).value();
    }
    for (int i = 499; i >= 0; --i)
    {
        REQUIRE(b.addStream(parent, toUtf16(std::format("item{}", i)).value(), bytesOf(std::format("value {}", i))));
    }
    auto c = openMemory(writeToMemory(b).value());
    REQUIRE(c);

    std::string path;
    for (int depth = 0; depth < 20; ++depth)
    {
        path += std::format("level{}/", depth);
    }
    const auto leafDir = c->findPath(path);
    REQUIRE(leafDir);
    const auto& children = c->entry(*leafDir).children;
    REQUIRE(children.size() == 500);
    for (std::size_t i = 1; i < children.size(); ++i)
    {
        CHECK(std::is_lt(compareNames(c->entry(children[i - 1]).name, c->entry(children[i]).name)));
    }
    CHECK(streamContents(*c, path + "ITEM42") == bytesOf("value 42"));
    CHECK_FALSE(c->findPath(path + "missing"));
}

TEST_CASE("Files needing DIFAT sectors round-trip", "[cfb][container]")
{
    Builder b(Version::v3);
    const auto big = patternBytes(std::size_t{ 8 } << 20, 9);
    REQUIRE(b.addStream(Builder::root(), u"essence", big));
    auto c = openMemory(writeToMemory(b).value());
    REQUIRE(c);
    CHECK(c->header().fatSectors > kHeaderDifatEntries);
    CHECK(c->header().difatSectors > 0);
    CHECK(streamContents(*c, "essence") == big);
}

TEST_CASE("fromContainer copies a tree, including across versions", "[cfb][container]")
{
    Builder b(Version::v3);
    const auto s = b.addStorage(Builder::root(), u"Header-2").value();
    REQUIRE(b.addStream(s, u"properties", patternBytes(100, 1)));
    REQUIRE(b.addStream(s, u"data", patternBytes(10000, 2)));
    b.node(Builder::root()).clsid = parseClsid("{b3b398a5-1c90-11d4-8053-080036210804}").value();
    b.setHeaderClsid(parseClsid("{42464141-000d-4d4f-060e-2b34010101ff}").value());
    auto original = openMemory(writeToMemory(b).value());
    REQUIRE(original);

    const auto version = GENERATE(Version::v3, Version::v4);
    auto copy = Builder::fromContainer(*original);
    REQUIRE(copy);
    copy->setVersion(version);
    auto reopened = openMemory(writeToMemory(*copy).value());
    REQUIRE(reopened);
    CHECK(reopened->header().version == version);
    CHECK(diffTrees(*original, *reopened).empty());
    CHECK(toString(reopened->header().clsid) == "{42464141-000d-4d4f-060e-2b34010101ff}");
}

TEST_CASE("writeFile replaces the target atomically", "[cfb][container]")
{
    const auto dir = std::filesystem::temp_directory_path() / std::format("aaf-test-{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    const auto path = dir / "out.aaf";

    Builder b;
    REQUIRE(b.addStream(Builder::root(), u"one", bytesOf("1")));
    REQUIRE(writeFile(b, path));
    REQUIRE(b.addStream(Builder::root(), u"two", bytesOf("22")));
    REQUIRE(writeFile(b, path));

    auto c = Container::openFile(path);
    REQUIRE(c);
    CHECK(streamContents(*c, "two") == bytesOf("22"));
    CHECK(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1);

    Builder bad;
    REQUIRE(bad.addStream(Builder::root(), u"x", bytesOf("x")));
    const auto failing = [](ByteSink&) -> aaf::Result<void> { return aaf::fail(aaf::Errc::io, "boom"); };
    CHECK_FALSE(writeFileAtomic(path, failing));
    CHECK(Container::openFile(path));
    CHECK(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1);
    std::filesystem::remove_all(dir);
}
