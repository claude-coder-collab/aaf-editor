#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace aaf::detail
{

enum class GenKind : std::uint8_t
{
    integer,
    enumeration,
    record,
    fixed_array,
    var_array,
    rename,
    string,
    stream,
    opaque,
    ext_enum,
    character,
    generic_character,
    indirect,
    set,
    strong_ref,
    weak_ref,
};

struct GenProperty
{
    std::string_view id;
    std::string_view name;
    std::string_view type;
    std::uint16_t pid;
    bool optional;
    bool unique;
};

struct GenClass
{
    std::string_view id;
    std::string_view name;
    std::string_view parent;
    bool concrete;
    std::span<const GenProperty> properties;
};

struct GenNamed
{
    std::string_view name;
    std::string_view value;
};

struct GenEnumValue
{
    std::string_view name;
    std::int64_t value;
};

struct GenType
{
    std::string_view id;
    std::string_view name;
    std::string_view element;
    std::span<const GenNamed> fields;
    std::span<const GenEnumValue> enumValues;
    std::span<const GenNamed> extValues;
    std::span<const std::string_view> path;
    std::uint32_t count;
    GenKind kind;
    std::uint8_t size;
    bool isSigned;
};

[[nodiscard]] auto baselineClasses() noexcept -> std::span<const GenClass>;
[[nodiscard]] auto baselineTypes() noexcept -> std::span<const GenType>;

}
