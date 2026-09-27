#pragma once

#include <aaf/core/auid.hpp>
#include <aaf/core/metamodel.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace aaf
{

/// A decoded property value.
class Value
{
public:
    struct Enum
    {
        std::int64_t value = 0;
        /// Element name, empty if the value is not a defined element.
        std::string name;
    };
    struct ExtEnum
    {
        Auid value;
        std::string name;
    };
    struct Record
    {
        std::vector<std::string> names;
        std::vector<Value> values;
    };
    using Array = std::vector<Value>;
    struct Indirect
    {
        Auid type;
        /// Exactly one element: the contained value.
        std::vector<Value> value;
    };
    struct Opaque
    {
        Auid type;
        std::vector<std::byte> bytes;
    };
    using Bytes = std::vector<std::byte>;

    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, std::string, Auid, MobId, Enum, ExtEnum, Record, Array, Indirect, Opaque, Bytes>;

    Value() = default;
    template <typename T>
        requires std::constructible_from<Storage, T&&>
    Value(T&& v) :
        data(std::forward<T>(v))
    {
    }

    template <typename T>
    [[nodiscard]] auto is() const noexcept -> bool
    {
        return std::holds_alternative<T>(data);
    }
    template <typename T>
    [[nodiscard]] auto as() const -> const T&
    {
        return std::get<T>(data);
    }
    /// Human-readable single-line representation.
    [[nodiscard]] auto toString() const -> std::string;

    Storage data;
};

/// Decodes stored bytes of type `type` in the given byte order.
[[nodiscard]] auto decodeValue(const MetaModel& model, const Auid& type, std::span<const std::byte> bytes, bool bigEndian) -> Result<Value>;

}
