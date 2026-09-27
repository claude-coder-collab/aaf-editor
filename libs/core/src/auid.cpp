#include <aaf/core/auid.hpp>

#include <algorithm>
#include <format>
#include <random>

namespace aaf
{

namespace
{

constexpr std::array<std::size_t, 16> kDisplayOrder = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };

void swapGuidFields(std::span<std::byte, 16> b) noexcept
{
    std::reverse(b.begin(), b.begin() + 4);
    std::reverse(b.begin() + 4, b.begin() + 6);
    std::reverse(b.begin() + 6, b.begin() + 8);
}

auto hexValue(char c) -> int
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

auto u8(const std::array<std::byte, 32>& b, std::size_t i) -> unsigned
{
    return std::to_integer<unsigned>(b[i]);
}

}

auto Auid::fromStored(std::span<const std::byte, 16> data, bool bigEndian) noexcept -> Auid
{
    Auid auid;
    std::ranges::copy(data, auid.bytes.begin());
    if (bigEndian)
    {
        swapGuidFields(auid.bytes);
    }
    return auid;
}

auto Auid::toStored(bool bigEndian) const noexcept -> std::array<std::byte, 16>
{
    auto out = bytes;
    if (bigEndian)
    {
        swapGuidFields(out);
    }
    return out;
}

auto Auid::parse(std::string_view text) -> Result<Auid>
{
    if (text.starts_with("urn:uuid:"))
    {
        text.remove_prefix(9);
    }
    if (text.size() >= 2 && text.front() == '{' && text.back() == '}')
    {
        text = text.substr(1, text.size() - 2);
    }
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    {
        return fail(Errc::invalid_argument, std::format("malformed AUID '{}'", text));
    }
    Auid auid;
    std::size_t pos = 0;
    for (const auto index : kDisplayOrder)
    {
        if (text[pos] == '-')
        {
            ++pos;
        }
        const int hi = hexValue(text[pos]);
        const int lo = hexValue(text[pos + 1]);
        if (hi < 0 || lo < 0)
        {
            return fail(Errc::invalid_argument, std::format("malformed AUID '{}'", text));
        }
        auid.bytes[index] = static_cast<std::byte>(hi * 16 + lo);
        pos += 2;
    }
    return auid;
}

auto Auid::toString() const -> std::string
{
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < kDisplayOrder.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            out.push_back('-');
        }
        out += std::format("{:02x}", std::to_integer<unsigned>(bytes[kDisplayOrder[i]]));
    }
    return out;
}

auto MobId::fromStored(std::span<const std::byte, 32> data, bool bigEndian) noexcept -> MobId
{
    MobId id;
    std::ranges::copy(data, id.bytes.begin());
    if (bigEndian)
    {
        swapGuidFields(std::span<std::byte, 16>(id.bytes.data() + 16, 16));
    }
    return id;
}

auto MobId::toStored(bool bigEndian) const noexcept -> std::array<std::byte, 32>
{
    auto out = bytes;
    if (bigEndian)
    {
        swapGuidFields(std::span<std::byte, 16>(out.data() + 16, 16));
    }
    return out;
}

auto Auid::generate() -> Auid
{
    static thread_local std::mt19937_64 engine(std::random_device{}() ^ (std::uint64_t{ std::random_device{}() } << 32U));
    Auid id;
    for (std::size_t i = 0; i < id.bytes.size(); i += 8)
    {
        const auto v = engine();
        for (std::size_t k = 0; k < 8; ++k)
        {
            id.bytes[i + k] = static_cast<std::byte>((v >> (k * 8)) & 0xFFU);
        }
    }
    id.bytes[7] = (id.bytes[7] & std::byte{ 0x0F }) | std::byte{ 0x40 };
    id.bytes[8] = (id.bytes[8] & std::byte{ 0x3F }) | std::byte{ 0x80 };
    return id;
}

auto MobId::generate() -> MobId
{
    constexpr std::array<std::uint8_t, 12> kLabel = { 0x06, 0x0a, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x0f, 0x20 };
    MobId id;
    for (std::size_t i = 0; i < kLabel.size(); ++i)
    {
        id.bytes[i] = std::byte{ kLabel[i] };
    }
    id.bytes[12] = std::byte{ 0x13 };
    const auto material = Auid::generate();
    std::ranges::copy(material.bytes, id.bytes.begin() + 16);
    return id;
}

auto MobId::parse(std::string_view text) -> Result<MobId>
{
    constexpr std::string_view kPrefix = "urn:smpte:umid:";
    if (text.starts_with(kPrefix))
    {
        text.remove_prefix(kPrefix.size());
    }
    std::string hex;
    for (const char c : text)
    {
        if (c != '.' && c != '-')
        {
            hex.push_back(c);
        }
    }
    if (hex.size() != 64 || std::ranges::any_of(hex, [](char c) -> bool { return hexValue(c) < 0; }))
    {
        return fail(Errc::invalid_argument, std::format("malformed MobID '{}'", text));
    }
    auto byteAt = [&hex](std::size_t i) -> std::byte { return static_cast<std::byte>(hexValue(hex[i * 2]) * 16 + hexValue(hex[i * 2 + 1])); };
    MobId id;
    for (std::size_t i = 0; i < 16; ++i)
    {
        id.bytes[i] = byteAt(i);
    }
    std::array<std::byte, 16> text16{};
    for (std::size_t i = 0; i < 16; ++i)
    {
        text16[i] = byteAt(16 + i);
    }
    const bool swappedMaterial = id.bytes[11] == std::byte{ 0 } && text16[0] == std::byte{ 0x06 } && text16[1] == std::byte{ 0x0E } && text16[2] == std::byte{ 0x2B } && text16[3] == std::byte{ 0x34 } && text16[4] == std::byte{ 0x7F } && text16[5] == std::byte{ 0x7F };
    std::array<std::byte, 8> data4{};
    std::array<std::byte, 8> fields{};
    if (swappedMaterial)
    {
        std::copy_n(text16.begin(), 8, data4.begin());
        std::copy_n(text16.begin() + 8, 8, fields.begin());
    }
    else
    {
        std::copy_n(text16.begin(), 8, fields.begin());
        std::copy_n(text16.begin() + 8, 8, data4.begin());
    }
    id.bytes[16] = fields[3];
    id.bytes[17] = fields[2];
    id.bytes[18] = fields[1];
    id.bytes[19] = fields[0];
    id.bytes[20] = fields[5];
    id.bytes[21] = fields[4];
    id.bytes[22] = fields[7];
    id.bytes[23] = fields[6];
    std::ranges::copy(data4, id.bytes.begin() + 24);
    return id;
}

auto MobId::toString() const -> std::string
{
    const auto& b = bytes;
    std::string out = "urn:smpte:umid:";
    for (std::size_t i = 0; i < 12; ++i)
    {
        out += std::format("{:02x}", u8(b, i));
        if (i % 4 == 3)
        {
            out.push_back('.');
        }
    }
    out += std::format("{:02x}{:02x}{:02x}{:02x}.", u8(b, 12), u8(b, 13), u8(b, 14), u8(b, 15));
    const unsigned data1 = u8(b, 16) | (u8(b, 17) << 8U) | (u8(b, 18) << 16U) | (u8(b, 19) << 24U);
    const unsigned data2 = u8(b, 20) | (u8(b, 21) << 8U);
    const unsigned data3 = u8(b, 22) | (u8(b, 23) << 8U);
    auto data4 = [&b](std::size_t first) -> std::string {
        return std::format("{:02x}{:02x}{:02x}{:02x}", u8(b, 24 + first), u8(b, 25 + first), u8(b, 26 + first), u8(b, 27 + first));
    };
    const bool swappedMaterial = u8(b, 11) == 0x00 && u8(b, 24) == 0x06 && u8(b, 25) == 0x0E && u8(b, 26) == 0x2B && u8(b, 27) == 0x34 && u8(b, 28) == 0x7F && u8(b, 29) == 0x7F;
    if (swappedMaterial)
    {
        out += std::format("{}.{}.{:08x}.{:04x}{:04x}", data4(0), data4(4), data1, data2, data3);
    }
    else
    {
        out += std::format("{:08x}.{:04x}{:04x}.{}.{}", data1, data2, data3, data4(0), data4(4));
    }
    return out;
}

}
