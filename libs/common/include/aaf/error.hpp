#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace aaf
{

/// Broad category of a library error.
enum class Errc : std::uint8_t {
    io,
    format,
    unsupported,
    limit,
    invalid_argument,
    not_found,
};

/// Error returned by all fallible library operations.
struct Error
{
    Errc code = Errc::format;
    std::string message;
    /// Byte offset in the source file the error relates to, when known.
    std::optional<std::uint64_t> offset;
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline auto fail(Errc code, std::string message, std::optional<std::uint64_t> offset = std::nullopt) -> std::unexpected<Error>
{
    return std::unexpected(Error{ code, std::move(message), offset });
}

[[nodiscard]] constexpr auto to_string(Errc code) noexcept -> std::string_view
{
    switch (code)
    {
        case Errc::io:
            return "I/O error";
        case Errc::format:
            return "format error";
        case Errc::unsupported:
            return "unsupported";
        case Errc::limit:
            return "limit exceeded";
        case Errc::invalid_argument:
            return "invalid argument";
        case Errc::not_found:
            return "not found";
    }
    return "unknown error";
}

[[nodiscard]] inline auto to_string(const Error& error) -> std::string
{
    if (error.offset)
    {
        return std::format("{}: {} (at offset {:#x})", to_string(error.code), error.message, *error.offset);
    }
    return std::format("{}: {}", to_string(error.code), error.message);
}

}
