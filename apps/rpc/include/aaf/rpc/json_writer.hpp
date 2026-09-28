#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

namespace aaf::rpc
{

/// Appends compact JSON text directly to a string, for large results where building a JSON tree first is too slow.
/// Commas are inserted automatically; the caller is responsible for balanced begin/end calls.
class JsonWriter
{
public:
    auto beginObject() -> JsonWriter&;
    auto endObject() -> JsonWriter&;
    auto beginArray() -> JsonWriter&;
    auto endArray() -> JsonWriter&;
    /// Writes an object key; the next call writes its value.
    auto key(std::string_view name) -> JsonWriter&;
    auto value(std::string_view text) -> JsonWriter&;
    auto value(const char* text) -> JsonWriter& { return value(std::string_view(text)); }
    auto value(bool flag) -> JsonWriter&;

    template <std::integral T>
        requires(!std::same_as<T, bool>)
    auto value(T number) -> JsonWriter&
    {
        separate();
        std::array<char, 24> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
        out_.append(buffer.data(), result.ptr);
        return *this;
    }
    auto null() -> JsonWriter&;
    /// Writes already-serialised JSON as a value.
    auto raw(std::string_view json) -> JsonWriter&;

    [[nodiscard]] auto str() const noexcept -> const std::string& { return out_; }
    [[nodiscard]] auto take() noexcept -> std::string { return std::move(out_); }

private:
    void separate();
    void appendString(std::string_view text);

    std::string out_;
};

}
