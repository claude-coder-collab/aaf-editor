#include "fixtures.hpp"

#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace aaf::cfb;
using namespace aaf::test;

namespace
{

constexpr std::string_view kAafSignature512 = "{42464141-000d-4d4f-060e-2b34010101ff}";
constexpr std::string_view kAafSignature4K = "{0d010201-0200-0000-060e-2b3403020101}";
constexpr std::string_view kRootClassAuid = "{b3b398a5-1c90-11d4-8053-080036210804}";

void checkRoundTrip(const std::filesystem::path& path)
{
    INFO(path.string());
    auto original = Container::openFile(path);
    REQUIRE(original);
    for (const auto version : { Version::v3, Version::v4 })
    {
        auto builder = Builder::fromContainer(*original);
        REQUIRE(builder);
        builder->setVersion(version);
        auto bytes = writeToMemory(*builder);
        REQUIRE(bytes);
        auto copy = openMemory(std::move(*bytes));
        REQUIRE(copy);
        CHECK(diffTrees(*original, *copy).empty());
        CHECK(copy->header().clsid == original->header().clsid);
    }
}

}

TEST_CASE("AAF SDK reference files open as AAF compound files", "[cfb][fixtures]")
{
    const auto files = aafFilesIn("aafsdk");
    REQUIRE(files.size() == 6);
    for (const auto& path : files)
    {
        INFO(path.string());
        auto c = Container::openFile(path);
        REQUIRE(c);
        const auto signature = toString(c->header().clsid);
        CHECK((signature == kAafSignature512 || signature == kAafSignature4K));
        CHECK(toString(c->root().clsid) == kRootClassAuid);
        CHECK(c->findPath("properties"));
        CHECK(c->findPath("referenced properties"));
        CHECK(c->findPath("Header-2/properties"));
    }
}

TEST_CASE("AAF SDK reference files round-trip at the container level", "[cfb][fixtures]")
{
    for (const auto& path : aafFilesIn("aafsdk"))
    {
        checkRoundTrip(path);
    }
}

TEST_CASE("External reference files round-trip at the container level", "[cfb][fixtures][external]")
{
    const auto files = aafFilesIn("external");
    if (files.empty())
    {
        SKIP("external fixtures not fetched; run tools/fetch_fixtures.py");
    }
    for (const auto& path : files)
    {
        checkRoundTrip(path);
    }
}
