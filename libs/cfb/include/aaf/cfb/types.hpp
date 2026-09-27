#pragma once

#include <aaf/error.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aaf::cfb
{

inline constexpr std::uint32_t kMaxRegSect = 0xFFFFFFFA;
inline constexpr std::uint32_t kDifSect = 0xFFFFFFFC;
inline constexpr std::uint32_t kFatSect = 0xFFFFFFFD;
inline constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;
inline constexpr std::uint32_t kFreeSect = 0xFFFFFFFF;
inline constexpr std::uint32_t kNoStream = 0xFFFFFFFF;

inline constexpr std::uint32_t kMiniStreamCutoff = 4096;
inline constexpr std::uint32_t kMiniSectorSize = 64;
inline constexpr std::uint32_t kDirEntrySize = 128;
inline constexpr std::size_t kMaxNameLength = 31;
inline constexpr std::size_t kHeaderDifatEntries = 109;

/// Major version of the compound file: v3 uses 512-byte sectors, v4 uses 4096-byte sectors.
enum class Version : std::uint16_t {
    v3 = 3,
    v4 = 4,
};

[[nodiscard]] constexpr auto sectorSize(Version v) noexcept -> std::uint32_t
{
    return v == Version::v3 ? 512U : 4096U;
}

enum class EntryType : std::uint8_t {
    empty = 0,
    storage = 1,
    stream = 2,
    root = 5,
};

using EntryId = std::uint32_t;

/// A CLSID as its 16 raw on-disk bytes.
struct Clsid
{
    std::array<std::byte, 16> bytes{};

    [[nodiscard]] auto isNull() const noexcept -> bool { return *this == Clsid{}; }
    auto operator<=>(const Clsid&) const = default;
};

/// Formats a CLSID in registry form, e.g. `{42464141-000d-4d4f-060e-2b34010101ff}`.
[[nodiscard]] auto toString(const Clsid& clsid) -> std::string;
/// Parses a CLSID in registry form (braces optional).
[[nodiscard]] auto parseClsid(std::string_view text) -> Result<Clsid>;

/// Converts UTF-16 to UTF-8, replacing unpaired surrogates with U+FFFD.
[[nodiscard]] auto toUtf8(std::u16string_view text) -> std::string;
/// Converts UTF-8 to UTF-16; fails on malformed input.
[[nodiscard]] auto toUtf16(std::string_view text) -> Result<std::u16string>;

/// Upper-cases a UTF-16 code unit the way compound file directory ordering requires.
[[nodiscard]] auto upperCase(char16_t c) noexcept -> char16_t;
/// Orders directory entry names as [MS-CFB] requires: shorter names first, then by upper-cased code units.
[[nodiscard]] auto compareNames(std::u16string_view a, std::u16string_view b) noexcept -> std::strong_ordering;
/// Checks that `name` is a legal directory entry name.
[[nodiscard]] auto validateName(std::u16string_view name) -> Result<void>;

}
