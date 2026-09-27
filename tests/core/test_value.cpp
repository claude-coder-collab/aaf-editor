#include <aaf/core/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <vector>

using namespace aaf;
using namespace aaf::literals;

namespace
{

auto bytes(std::initializer_list<unsigned> values) -> std::vector<std::byte>
{
    std::vector<std::byte> out;
    for (const auto v : values)
    {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

auto utf16(std::u16string_view text) -> std::vector<std::byte>
{
    std::vector<std::byte> out;
    for (const auto c : text)
    {
        out.push_back(static_cast<std::byte>(c & 0xFF));
        out.push_back(static_cast<std::byte>(c >> 8));
    }
    return out;
}

const auto& model()
{
    return MetaModel::baseline();
}

constexpr auto kInt16 = "01010600-0000-0000-060e-2b3401040101"_auid;
constexpr auto kUInt32 = "01010300-0000-0000-060e-2b3401040101"_auid;
constexpr auto kRational = "03010100-0000-0000-060e-2b3401040101"_auid;
constexpr auto kStringArray = "04010500-0000-0000-060e-2b3401040101"_auid;
constexpr auto kIndirect = "04100300-0000-0000-060e-2b3401040101"_auid;
constexpr auto kProductReleaseType = "02010101-0000-0000-060e-2b3401040101"_auid;
constexpr auto kInt32Array = "04010300-0000-0000-060e-2b3401040101"_auid;
constexpr auto kUInt8Array8 = "04010800-0000-0000-060e-2b3401040101"_auid;

}

TEST_CASE("Integers decode in both byte orders with sign extension", "[core][value]")
{
    CHECK(decodeValue(model(), kInt16, bytes({ 0xFE, 0xFF }), false).value().as<std::int64_t>() == -2);
    CHECK(decodeValue(model(), kInt16, bytes({ 0xFF, 0xFE }), true).value().as<std::int64_t>() == -2);
    CHECK(decodeValue(model(), kUInt32, bytes({ 0x78, 0x56, 0x34, 0x12 }), false).value().as<std::uint64_t>() == 0x12345678);
    CHECK_FALSE(decodeValue(model(), kUInt32, bytes({ 1, 2, 3 }), false));
}

TEST_CASE("Enumerations decode to named elements", "[core][value]")
{
    CHECK(decodeValue(model(), ids::kTypeBoolean, bytes({ 1 }), false).value().as<bool>());
    const auto released = decodeValue(model(), kProductReleaseType, bytes({ 1 }), false).value();
    CHECK(released.as<Value::Enum>().name == "VersionReleased");
    const auto unknown = decodeValue(model(), kProductReleaseType, bytes({ 99 }), false).value();
    CHECK(unknown.as<Value::Enum>().name.empty());
    CHECK(unknown.toString() == "99");
}

TEST_CASE("Strings and string arrays decode from UTF-16", "[core][value]")
{
    CHECK(decodeValue(model(), ids::kTypeString, utf16(u"Mob é\0"), false).value().as<std::string>() == "Mob \xc3\xa9");
    const auto list = decodeValue(model(), kStringArray, utf16(std::u16string(u"one\0two\0", 8)), false).value().as<Value::Array>();
    REQUIRE(list.size() == 2);
    CHECK(list[1].as<std::string>() == "two");
}

TEST_CASE("Records, arrays and special records decode", "[core][value]")
{
    const auto rate = decodeValue(model(), kRational, bytes({ 25, 0, 0, 0, 1, 0, 0, 0 }), false).value().as<Value::Record>();
    REQUIRE(rate.names.size() == 2);
    CHECK(rate.names[0] == "Numerator");
    CHECK(rate.values[0].as<std::int64_t>() == 25);
    CHECK_FALSE(decodeValue(model(), kRational, bytes({ 25, 0, 0, 0 }), false));

    const auto id = "0d010101-0101-2f00-060e-2b3402060101"_auid;
    CHECK(decodeValue(model(), ids::kTypeAuid, id.bytes, false).value().as<Auid>() == id);

    const auto ints = decodeValue(model(), kInt32Array, bytes({ 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF }), false).value().as<Value::Array>();
    REQUIRE(ints.size() == 2);
    CHECK(ints[1].as<std::int64_t>() == -1);
    CHECK_FALSE(decodeValue(model(), kInt32Array, bytes({ 1, 0, 0 }), false));
    CHECK(decodeValue(model(), kUInt8Array8, bytes({ 1, 2, 3, 4, 5, 6, 7, 8 }), false).value().as<Value::Array>().size() == 8);
    CHECK_FALSE(decodeValue(model(), kUInt8Array8, bytes({ 1, 2 }), false));
}

TEST_CASE("Indirect values carry their own type", "[core][value]")
{
    auto data = bytes({ 0x4C });
    const auto type = kUInt32;
    data.insert(data.end(), type.bytes.begin(), type.bytes.end());
    data.insert(data.end(), { std::byte{ 42 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 } });
    const auto v = decodeValue(model(), kIndirect, data, false).value();
    CHECK(v.as<Value::Indirect>().type == type);
    CHECK(v.toString() == "42");
    CHECK_FALSE(decodeValue(model(), kIndirect, bytes({ 0x4C, 1 }), false));
}

TEST_CASE("Unknown types fail to decode", "[core][value]")
{
    CHECK_FALSE(decodeValue(model(), "00000000-0000-0000-0000-000000000001"_auid, bytes({ 1 }), false));
}
