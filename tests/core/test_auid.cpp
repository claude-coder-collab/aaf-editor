#include <aaf/core/auid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace aaf;
using namespace aaf::literals;

TEST_CASE("AUIDs parse, format and compare", "[core][auid]")
{
    constexpr auto header = "0d010101-0101-2f00-060e-2b3402060101"_auid;
    CHECK(header.toString() == "0d010101-0101-2f00-060e-2b3402060101");
    CHECK(header.bytes[0] == std::byte{ 0x01 });
    CHECK(header.bytes[3] == std::byte{ 0x0d });
    CHECK(Auid::parse("{0D010101-0101-2F00-060E-2B3402060101}").value() == header);
    CHECK(Auid::parse("urn:uuid:0d010101-0101-2f00-060e-2b3402060101").value() == header);
    CHECK_FALSE(Auid::parse("0d010101-0101-2f00-060e-2b340206010"));
    CHECK_FALSE(Auid::parse("0d010101-0101-2f00-060e-2b34020601zz"));
    CHECK(Auid{}.isNull());
    CHECK(std::hash<Auid>{}(header) != std::hash<Auid>{}(Auid{}));
}

TEST_CASE("AUIDs decode from both byte orders", "[core][auid]")
{
    const auto le = "0d010101-0101-2f00-060e-2b3402060101"_auid;
    std::array<std::byte, 16> big = le.bytes;
    std::swap(big[0], big[3]);
    std::swap(big[1], big[2]);
    std::swap(big[4], big[5]);
    std::swap(big[6], big[7]);
    CHECK(Auid::fromStored(big, true) == le);
    CHECK(Auid::fromStored(le.bytes, false) == le);
}

TEST_CASE("MobIDs format as SMPTE UMID URNs like pyaaf2", "[core][auid]")
{
    const std::array<unsigned, 32> raw = { 0x06, 0x0c, 0x2b, 0x34, 0x02, 0x05, 0x11, 0x01, 0x01, 0x00, 0x10, 0x00, 0x13, 0x00, 0x00, 0x00, 0x10, 0xf1, 0x9a, 0xca, 0x00, 0x04, 0xd4, 0x11, 0x8e, 0x3d, 0x00, 0x90, 0x27, 0xdf, 0xca, 0x7c };
    MobId id;
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        id.bytes[i] = static_cast<std::byte>(raw[i]);
    }
    CHECK(id.toString() == "urn:smpte:umid:060c2b34.02051101.01001000.13000000.ca9af110.040011d4.8e3d0090.27dfca7c");

    MobId swapped = id;
    const std::array<unsigned, 8> data4 = { 0x06, 0x0e, 0x2b, 0x34, 0x7f, 0x7f, 0x2a, 0x80 };
    for (std::size_t i = 0; i < data4.size(); ++i)
    {
        swapped.bytes[24 + i] = static_cast<std::byte>(data4[i]);
    }
    CHECK(swapped.toString() == "urn:smpte:umid:060c2b34.02051101.01001000.13000000.060e2b34.7f7f2a80.ca9af110.040011d4");
}

TEST_CASE("MobIDs parse from their URN form and can be generated", "[core][auid]")
{
    for (const auto* urn : { "urn:smpte:umid:060c2b34.02051101.01001000.13000000.ca9af110.040011d4.8e3d0090.27dfca7c",
             "urn:smpte:umid:060c2b34.02051101.01001000.13000000.060e2b34.7f7f2a80.ca9af110.040011d4" })
    {
        const auto id = MobId::parse(urn);
        REQUIRE(id);
        CHECK(id->toString() == urn);
    }
    CHECK_FALSE(MobId::parse("urn:smpte:umid:060c2b34"));
    CHECK_FALSE(MobId::parse("urn:smpte:umid:zz0c2b34.02051101.01001000.13000000.ca9af110.040011d4.8e3d0090.27dfca7c"));

    const auto a = MobId::generate();
    const auto b = MobId::generate();
    CHECK(a != b);
    CHECK(MobId::parse(a.toString()).value() == a);
    CHECK(a.toString().starts_with("urn:smpte:umid:060a2b34.01010105.01010f20.13000000."));

    const auto u = Auid::generate();
    CHECK(u != Auid::generate());
    CHECK(u.toString()[14] == '4');
}
