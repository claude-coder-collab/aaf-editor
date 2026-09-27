#include <aaf/cfb/types.hpp>
#include <aaf/core/value.hpp>

#include <algorithm>
#include <format>
#include <limits>

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

void writeUnsigned(std::vector<std::byte>& out, std::uint64_t v, std::size_t size, bool bigEndian)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        const std::size_t shift = (bigEndian ? size - 1 - i : i) * 8;
        out.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
    }
}

void writeUtf16(std::vector<std::byte>& out, std::u16string_view text, bool bigEndian)
{
    for (const auto c : text)
    {
        writeUnsigned(out, c, 2, bigEndian);
    }
}

auto typeError(const TypeDef& t, std::string_view expected) -> std::unexpected<Error>
{
    return fail(Errc::invalid_argument, std::format("value for type {} must be {}", t.name, expected));
}

auto integerOf(const Value& v) -> std::optional<std::pair<bool, std::uint64_t>>
{
    if (v.is<std::int64_t>())
    {
        const auto x = v.as<std::int64_t>();
        return std::pair{ x < 0, static_cast<std::uint64_t>(x) };
    }
    if (v.is<std::uint64_t>())
    {
        return std::pair{ false, v.as<std::uint64_t>() };
    }
    if (v.is<Value::Enum>())
    {
        const auto x = v.as<Value::Enum>().value;
        return std::pair{ x < 0, static_cast<std::uint64_t>(x) };
    }
    return std::nullopt;
}

auto encodeInteger(const TypeDef& t, const Value& v, bool be, std::vector<std::byte>& out) -> Result<void>
{
    const auto n = integerOf(v);
    if (!n)
    {
        return typeError(t, "an integer");
    }
    const auto [negative, bits] = *n;
    const unsigned width = t.size * 8U;
    if (t.isSigned)
    {
        const auto x = static_cast<std::int64_t>(bits);
        const std::int64_t min = width >= 64 ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t{ 1 } << (width - 1));
        const std::int64_t max = width >= 64 ? std::numeric_limits<std::int64_t>::max() : (std::int64_t{ 1 } << (width - 1)) - 1;
        if ((!negative && bits > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) || x < min || x > max)
        {
            return fail(Errc::invalid_argument, std::format("value out of range for {}", t.name));
        }
    }
    else if (negative || (width < 64 && bits >= (std::uint64_t{ 1 } << width)))
    {
        return fail(Errc::invalid_argument, std::format("value out of range for {}", t.name));
    }
    writeUnsigned(out, bits, t.size, be);
    return {};
}

auto encodeImpl(const MetaModel& model, const Auid& typeId, const Value& v, bool be, int depth, std::vector<std::byte>& out) -> Result<void>;

auto encodeElements(const MetaModel& model, const TypeDef& t, const Value& v, bool be, int depth, std::vector<std::byte>& out) -> Result<void>
{
    if (!v.is<Value::Array>())
    {
        return typeError(t, "an array");
    }
    const auto& items = v.as<Value::Array>();
    if (t.kind == TypeKind::fixed_array && items.size() != t.count)
    {
        return fail(Errc::invalid_argument, std::format("{} needs exactly {} elements", t.name, t.count));
    }
    const auto* element = model.resolve(t.element);
    if (element == nullptr)
    {
        return fail(Errc::format, std::format("unknown element type of {}", t.name));
    }
    if (element->kind == TypeKind::character)
    {
        for (const auto& item : items)
        {
            if (!item.is<std::string>())
            {
                return typeError(t, "an array of strings");
            }
            auto text = cfb::toUtf16(item.as<std::string>());
            if (!text)
            {
                return std::unexpected(text.error());
            }
            writeUtf16(out, *text, be);
            writeUnsigned(out, 0, 2, be);
        }
        return {};
    }
    for (const auto& item : items)
    {
        if (auto r = encodeImpl(model, element->id, item, be, depth + 1, out); !r)
        {
            return r;
        }
    }
    return {};
}

auto encodeRecord(const MetaModel& model, const TypeDef& t, const Value& v, bool be, int depth, std::vector<std::byte>& out) -> Result<void>
{
    if (t.id == ids::kTypeAuid)
    {
        if (!v.is<Auid>())
        {
            return typeError(t, "an AUID");
        }
        const auto stored = v.as<Auid>().toStored(be);
        out.insert(out.end(), stored.begin(), stored.end());
        return {};
    }
    if (t.id == ids::kTypeMobId)
    {
        if (!v.is<MobId>())
        {
            return typeError(t, "a MobID");
        }
        const auto stored = v.as<MobId>().toStored(be);
        out.insert(out.end(), stored.begin(), stored.end());
        return {};
    }
    if (!v.is<Value::Record>())
    {
        return typeError(t, "a record");
    }
    const auto& record = v.as<Value::Record>();
    for (const auto& field : t.fields)
    {
        const auto it = std::ranges::find(record.names, field.name);
        if (it == record.names.end())
        {
            return fail(Errc::invalid_argument, std::format("record {} is missing field {}", t.name, field.name));
        }
        const auto& fieldValue = record.values[static_cast<std::size_t>(it - record.names.begin())];
        if (auto r = encodeImpl(model, field.type, fieldValue, be, depth + 1, out); !r)
        {
            return r;
        }
    }
    return {};
}

auto encodeImpl(const MetaModel& model, const Auid& typeId, const Value& v, bool be, int depth, std::vector<std::byte>& out) -> Result<void>
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
    if (v.is<Value::Bytes>() && t->kind != TypeKind::var_array && t->kind != TypeKind::fixed_array && t->kind != TypeKind::set)
    {
        const auto& raw = v.as<Value::Bytes>();
        out.insert(out.end(), raw.begin(), raw.end());
        return {};
    }
    switch (t->kind)
    {
        case TypeKind::integer:
            return encodeInteger(*t, v, be, out);
        case TypeKind::enumeration:
        {
            if (t->id == ids::kTypeBoolean)
            {
                if (!v.is<bool>())
                {
                    return typeError(*t, "a boolean");
                }
                out.push_back(std::byte{ v.as<bool>() ? std::uint8_t{ 1 } : std::uint8_t{ 0 } });
                return {};
            }
            std::optional<std::int64_t> number;
            if (v.is<Value::Enum>() && !v.as<Value::Enum>().name.empty())
            {
                const auto& name = v.as<Value::Enum>().name;
                const auto it = std::ranges::find(t->enumElements, name, &EnumElement::name);
                if (it == t->enumElements.end())
                {
                    return fail(Errc::invalid_argument, std::format("{} has no element {}", t->name, name));
                }
                number = it->value;
            }
            else if (const auto n = integerOf(v))
            {
                number = static_cast<std::int64_t>(n->second);
            }
            else if (v.is<std::string>())
            {
                const auto it = std::ranges::find(t->enumElements, v.as<std::string>(), &EnumElement::name);
                if (it == t->enumElements.end())
                {
                    return fail(Errc::invalid_argument, std::format("{} has no element {}", t->name, v.as<std::string>()));
                }
                number = it->value;
            }
            if (!number)
            {
                return typeError(*t, "an enumeration value");
            }
            const auto* element = model.resolve(t->element);
            if (element == nullptr || element->kind != TypeKind::integer)
            {
                return fail(Errc::format, std::format("enumeration {} has no integer element type", t->name));
            }
            return encodeInteger(*element, Value(*number), be, out);
        }
        case TypeKind::ext_enum:
        {
            Auid id;
            if (v.is<Auid>())
            {
                id = v.as<Auid>();
            }
            else if (v.is<Value::ExtEnum>() && !v.as<Value::ExtEnum>().value.isNull())
            {
                id = v.as<Value::ExtEnum>().value;
            }
            else
            {
                std::string name;
                if (v.is<Value::ExtEnum>())
                {
                    name = v.as<Value::ExtEnum>().name;
                }
                else if (v.is<std::string>())
                {
                    name = v.as<std::string>();
                }
                const auto it = std::ranges::find(t->extElements, name, &ExtEnumElement::name);
                if (name.empty() || it == t->extElements.end())
                {
                    return typeError(*t, "an AUID or element name");
                }
                id = it->value;
            }
            const auto stored = id.toStored(be);
            out.insert(out.end(), stored.begin(), stored.end());
            return {};
        }
        case TypeKind::record:
            return encodeRecord(model, *t, v, be, depth, out);
        case TypeKind::fixed_array:
        case TypeKind::var_array:
        case TypeKind::set:
            if (v.is<Value::Bytes>())
            {
                const auto& raw = v.as<Value::Bytes>();
                out.insert(out.end(), raw.begin(), raw.end());
                return {};
            }
            return encodeElements(model, *t, v, be, depth, out);
        case TypeKind::string:
        {
            if (!v.is<std::string>())
            {
                return typeError(*t, "a string");
            }
            const auto* element = model.resolve(t->element);
            if (element != nullptr && element->kind == TypeKind::generic_character && element->size == 1)
            {
                for (const char c : v.as<std::string>())
                {
                    out.push_back(static_cast<std::byte>(c));
                }
                out.push_back(std::byte{ 0 });
                return {};
            }
            auto text = cfb::toUtf16(v.as<std::string>());
            if (!text)
            {
                return std::unexpected(text.error());
            }
            writeUtf16(out, *text, be);
            writeUnsigned(out, 0, 2, be);
            return {};
        }
        case TypeKind::character:
        {
            auto text = v.is<std::string>() ? cfb::toUtf16(v.as<std::string>()) : Result<std::u16string>(std::u16string{});
            if (!text || text->size() != 1)
            {
                return typeError(*t, "a single character");
            }
            writeUtf16(out, *text, be);
            return {};
        }
        case TypeKind::generic_character:
        {
            const auto n = integerOf(v);
            if (!n)
            {
                return typeError(*t, "an integer");
            }
            writeUnsigned(out, n->second, t->size, be);
            return {};
        }
        case TypeKind::indirect:
        case TypeKind::opaque:
        {
            out.push_back(std::byte{ be ? std::uint8_t{ 0x42 } : std::uint8_t{ 0x4C } });
            if (v.is<Value::Opaque>())
            {
                const auto& o = v.as<Value::Opaque>();
                const auto stored = o.type.toStored(be);
                out.insert(out.end(), stored.begin(), stored.end());
                out.insert(out.end(), o.bytes.begin(), o.bytes.end());
                return {};
            }
            if (t->kind == TypeKind::opaque || !v.is<Value::Indirect>() || v.as<Value::Indirect>().value.size() != 1)
            {
                return typeError(*t, t->kind == TypeKind::opaque ? "an opaque value" : "an indirect value");
            }
            const auto& indirect = v.as<Value::Indirect>();
            const auto stored = indirect.type.toStored(be);
            out.insert(out.end(), stored.begin(), stored.end());
            return encodeImpl(model, indirect.type, indirect.value.front(), be, depth + 1, out);
        }
        case TypeKind::stream:
        case TypeKind::strong_ref:
        case TypeKind::weak_ref:
        case TypeKind::rename:
            break;
    }
    return typeError(*t, "raw bytes");
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

auto encodeValue(const MetaModel& model, const Auid& type, const Value& value, bool bigEndian) -> Result<std::vector<std::byte>>
{
    std::vector<std::byte> out;
    if (auto r = encodeImpl(model, type, value, bigEndian, 0, out); !r)
    {
        return std::unexpected(r.error());
    }
    return out;
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
