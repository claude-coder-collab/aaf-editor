#include <aaf/cfb/types.hpp>
#include <aaf/core/value.hpp>

#include <format>

namespace aaf
{

namespace
{

constexpr int kMaxDepth = 32;

auto readUnsigned(std::span<const std::byte> b, bool bigEndian) -> std::uint64_t
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < b.size(); ++i)
    {
        const std::size_t index = bigEndian ? i : b.size() - 1 - i;
        v = (v << 8U) | std::to_integer<std::uint64_t>(b[index]);
    }
    return v;
}

auto readSigned(std::span<const std::byte> b, bool bigEndian) -> std::int64_t
{
    const auto u = readUnsigned(b, bigEndian);
    const unsigned bits = static_cast<unsigned>(b.size()) * 8U;
    if (bits < 64 && (u >> (bits - 1)) != 0)
    {
        return static_cast<std::int64_t>(u | (~std::uint64_t{ 0 } << bits));
    }
    return static_cast<std::int64_t>(u);
}

auto decodeUtf16(std::span<const std::byte> b, bool bigEndian) -> std::u16string
{
    std::u16string out;
    out.reserve(b.size() / 2);
    for (std::size_t i = 0; i + 1 < b.size(); i += 2)
    {
        const auto c = static_cast<char16_t>(readUnsigned(b.subspan(i, 2), bigEndian));
        out.push_back(c);
    }
    return out;
}

auto sizeError(const TypeDef& t, std::size_t actual) -> std::unexpected<Error>
{
    return fail(Errc::format, std::format("value of type {} has invalid size {}", t.name, actual));
}

auto decodeImpl(const MetaModel& model, const Auid& typeId, std::span<const std::byte> b, bool be, int depth) -> Result<Value>;

auto decodeElements(const MetaModel& model, const TypeDef& t, std::span<const std::byte> b, bool be, int depth, std::optional<std::size_t> count) -> Result<Value>
{
    const auto* element = model.resolve(t.element);
    if (element == nullptr)
    {
        return fail(Errc::format, std::format("unknown element type {} of {}", t.element.toString(), t.name));
    }
    if (element->kind == TypeKind::character)
    {
        Value::Array strings;
        std::u16string current;
        for (const auto c : decodeUtf16(b, be))
        {
            if (c == 0)
            {
                strings.emplace_back(cfb::toUtf8(current));
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }
        if (!current.empty())
        {
            strings.emplace_back(cfb::toUtf8(current));
        }
        return Value(std::move(strings));
    }
    const auto size = model.fixedSize(element->id);
    if (!size || *size == 0)
    {
        return fail(Errc::unsupported, std::format("array {} has variable-size elements", t.name));
    }
    if (b.size() % *size != 0 || (count && b.size() != *count * *size))
    {
        return sizeError(t, b.size());
    }
    Value::Array items;
    items.reserve(b.size() / *size);
    for (std::size_t off = 0; off < b.size(); off += *size)
    {
        auto item = decodeImpl(model, element->id, b.subspan(off, *size), be, depth + 1);
        if (!item)
        {
            return item;
        }
        items.push_back(std::move(*item));
    }
    return Value(std::move(items));
}

auto decodeRecord(const MetaModel& model, const TypeDef& t, std::span<const std::byte> b, bool be, int depth) -> Result<Value>
{
    if (t.id == ids::kTypeAuid)
    {
        if (b.size() != 16)
        {
            return sizeError(t, b.size());
        }
        return Value(Auid::fromStored(b.first<16>(), be));
    }
    if (t.id == ids::kTypeMobId)
    {
        if (b.size() != 32)
        {
            return sizeError(t, b.size());
        }
        return Value(MobId::fromStored(b.first<32>(), be));
    }
    Value::Record record;
    std::size_t off = 0;
    for (const auto& field : t.fields)
    {
        const auto size = model.fixedSize(field.type);
        if (!size || off + *size > b.size())
        {
            return sizeError(t, b.size());
        }
        auto v = decodeImpl(model, field.type, b.subspan(off, *size), be, depth + 1);
        if (!v)
        {
            return v;
        }
        record.names.push_back(field.name);
        record.values.push_back(std::move(*v));
        off += *size;
    }
    if (off != b.size())
    {
        return sizeError(t, b.size());
    }
    return Value(std::move(record));
}

auto decodeImpl(const MetaModel& model, const Auid& typeId, std::span<const std::byte> b, bool be, int depth) -> Result<Value>
{
    if (depth > kMaxDepth)
    {
        return fail(Errc::limit, "type nesting too deep");
    }
    const auto* t = model.resolve(typeId);
    if (t == nullptr)
    {
        return fail(Errc::format, std::format("unknown type {}", typeId.toString()));
    }
    switch (t->kind)
    {
        case TypeKind::integer:
            if (b.size() != t->size || t->size == 0 || t->size > 8)
            {
                return sizeError(*t, b.size());
            }
            return t->isSigned ? Value(readSigned(b, be)) : Value(readUnsigned(b, be));
        case TypeKind::enumeration:
        {
            if (t->id == ids::kTypeBoolean)
            {
                if (b.size() != 1)
                {
                    return sizeError(*t, b.size());
                }
                return Value(b[0] != std::byte{ 0 });
            }
            auto raw = decodeImpl(model, t->element, b, be, depth + 1);
            if (!raw)
            {
                return raw;
            }
            Value::Enum e;
            e.value = raw->is<std::int64_t>() ? raw->as<std::int64_t>() : static_cast<std::int64_t>(raw->as<std::uint64_t>());
            for (const auto& element : t->enumElements)
            {
                if (element.value == e.value)
                {
                    e.name = element.name;
                    break;
                }
            }
            return Value(std::move(e));
        }
        case TypeKind::ext_enum:
        {
            if (b.size() != 16)
            {
                return sizeError(*t, b.size());
            }
            Value::ExtEnum e{ Auid::fromStored(b.first<16>(), be), {} };
            for (const auto& element : t->extElements)
            {
                if (element.value == e.value)
                {
                    e.name = element.name;
                    break;
                }
            }
            return Value(std::move(e));
        }
        case TypeKind::record:
            return decodeRecord(model, *t, b, be, depth);
        case TypeKind::fixed_array:
            return decodeElements(model, *t, b, be, depth, t->count);
        case TypeKind::var_array:
        case TypeKind::set:
            return decodeElements(model, *t, b, be, depth, std::nullopt);
        case TypeKind::string:
        {
            const auto* element = model.resolve(t->element);
            if (element != nullptr && element->kind == TypeKind::generic_character && element->size == 1)
            {
                std::string text;
                for (const auto c : b)
                {
                    if (c == std::byte{ 0 })
                    {
                        break;
                    }
                    text.push_back(static_cast<char>(c));
                }
                return Value(std::move(text));
            }
            auto text = decodeUtf16(b, be);
            if (const auto nul = text.find(u'\0'); nul != std::u16string::npos)
            {
                text.resize(nul);
            }
            return Value(cfb::toUtf8(text));
        }
        case TypeKind::character:
            if (b.size() != 2)
            {
                return sizeError(*t, b.size());
            }
            return Value(cfb::toUtf8(decodeUtf16(b, be)));
        case TypeKind::generic_character:
            if (b.size() != t->size || t->size == 0 || t->size > 8)
            {
                return sizeError(*t, b.size());
            }
            return Value(readUnsigned(b, be));
        case TypeKind::indirect:
        case TypeKind::opaque:
        {
            if (b.size() < 17)
            {
                return sizeError(*t, b.size());
            }
            const bool innerBe = b[0] == std::byte{ 0x42 };
            const auto inner = Auid::fromStored(b.subspan<1, 16>(), innerBe);
            const auto payload = b.subspan(17);
            if (t->kind == TypeKind::opaque)
            {
                return Value(Value::Opaque{ inner, { payload.begin(), payload.end() } });
            }
            auto v = decodeImpl(model, inner, payload, innerBe, depth + 1);
            if (!v)
            {
                return v;
            }
            Value::Indirect indirect{ inner, {} };
            indirect.value.push_back(std::move(*v));
            return Value(std::move(indirect));
        }
        case TypeKind::stream:
        case TypeKind::strong_ref:
        case TypeKind::weak_ref:
        case TypeKind::rename:
            break;
    }
    return Value(Value::Bytes(b.begin(), b.end()));
}

auto hex(std::span<const std::byte> bytes, std::size_t limit) -> std::string
{
    std::string out;
    for (std::size_t i = 0; i < bytes.size() && i < limit; ++i)
    {
        out += std::format("{:02x}", std::to_integer<unsigned>(bytes[i]));
    }
    if (bytes.size() > limit)
    {
        out += std::format("... ({} bytes)", bytes.size());
    }
    return out;
}

}

auto decodeValue(const MetaModel& model, const Auid& type, std::span<const std::byte> bytes, bool bigEndian) -> Result<Value>
{
    return decodeImpl(model, type, bytes, bigEndian, 0);
}

auto Value::toString() const -> std::string
{
    struct Visitor
    {
        auto operator()(std::monostate /*unused*/) const -> std::string { return "null"; }
        auto operator()(bool v) const -> std::string { return v ? "true" : "false"; }
        auto operator()(std::int64_t v) const -> std::string { return std::to_string(v); }
        auto operator()(std::uint64_t v) const -> std::string { return std::to_string(v); }
        auto operator()(const std::string& v) const -> std::string { return std::format("\"{}\"", v); }
        auto operator()(const Auid& v) const -> std::string { return v.toString(); }
        auto operator()(const MobId& v) const -> std::string { return v.toString(); }
        auto operator()(const Enum& v) const -> std::string { return v.name.empty() ? std::to_string(v.value) : v.name; }
        auto operator()(const ExtEnum& v) const -> std::string { return v.name.empty() ? v.value.toString() : v.name; }
        auto operator()(const Record& v) const -> std::string
        {
            std::string out = "{";
            for (std::size_t i = 0; i < v.names.size(); ++i)
            {
                out += std::format("{}{}: {}", i == 0 ? "" : ", ", v.names[i], v.values[i].toString());
            }
            return out + "}";
        }
        auto operator()(const Array& v) const -> std::string
        {
            std::string out = "[";
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                out += (i == 0 ? "" : ", ") + v[i].toString();
            }
            return out + "]";
        }
        auto operator()(const Indirect& v) const -> std::string { return v.value.empty() ? "null" : v.value.front().toString(); }
        auto operator()(const Opaque& v) const -> std::string { return std::format("opaque {} {}", v.type.toString(), hex(v.bytes, 32)); }
        auto operator()(const Bytes& v) const -> std::string { return hex(v, 32); }
    };
    return std::visit(Visitor{}, data);
}

}
