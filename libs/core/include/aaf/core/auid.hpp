#pragma once

#include <aaf/error.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace aaf
{

/// A 16-byte AAF identifier (UUID or byte-swapped SMPTE UL), held in its little-endian stored form.
struct Auid
{
    std::array<std::byte, 16> bytes{};

    /// Decodes a stored AUID; `bigEndian` selects the byte order of the Data1..Data3 fields.
    [[nodiscard]] static auto fromStored(std::span<const std::byte, 16> data, bool bigEndian = false) noexcept -> Auid;
    /// Parses `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`, with optional braces or `urn:uuid:` prefix.
    [[nodiscard]] static auto parse(std::string_view text) -> Result<Auid>;

    [[nodiscard]] auto isNull() const noexcept -> bool { return *this == Auid{}; }
    [[nodiscard]] auto toString() const -> std::string;
    auto operator<=>(const Auid&) const = default;
};

/// A 32-byte SMPTE UMID identifying a Mob, held in its little-endian stored form.
struct MobId
{
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] static auto fromStored(std::span<const std::byte, 32> data, bool bigEndian = false) noexcept -> MobId;
    /// Formats as `urn:smpte:umid:...`, matching the representation used by pyaaf2.
    [[nodiscard]] auto toString() const -> std::string;
    auto operator<=>(const MobId&) const = default;
};

namespace literals
{

[[nodiscard]] consteval auto operator""_auid(const char* text, std::size_t size) -> Auid
{
    auto hex = [](char c) -> int {
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
        throw "invalid hex digit in AUID literal";
    };
    if (size != 36)
    {
        throw "AUID literal must have 36 characters";
    }
    constexpr std::array<int, 16> order = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    Auid auid;
    std::size_t pos = 0;
    for (const int index : order)
    {
        if (text[pos] == '-')
        {
            ++pos;
        }
        auid.bytes[static_cast<std::size_t>(index)] = static_cast<std::byte>(hex(text[pos]) * 16 + hex(text[pos + 1]));
        pos += 2;
    }
    return auid;
}

}

}

template <>
struct std::hash<aaf::Auid>
{
    auto operator()(const aaf::Auid& auid) const noexcept -> std::size_t
    {
        std::size_t h = 1469598103934665603ULL;
        for (const auto b : auid.bytes)
        {
            h = (h ^ std::to_integer<std::size_t>(b)) * 1099511628211ULL;
        }
        return h;
    }
};
