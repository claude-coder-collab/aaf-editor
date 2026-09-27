#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>

namespace aaf::cfb::detail
{

template <std::unsigned_integral T>
[[nodiscard]] auto loadLE(std::span<const std::byte> data, std::size_t offset) noexcept -> T
{
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    if constexpr (std::endian::native == std::endian::big)
    {
        value = std::byteswap(value);
    }
    return value;
}

template <std::unsigned_integral T>
void storeLE(std::span<std::byte> data, std::size_t offset, T value) noexcept
{
    if constexpr (std::endian::native == std::endian::big)
    {
        value = std::byteswap(value);
    }
    std::memcpy(data.data() + offset, &value, sizeof(T));
}

}
