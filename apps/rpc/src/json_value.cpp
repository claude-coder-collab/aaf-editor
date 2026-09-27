#include <aaf/rpc/json_value.hpp>

#include <charconv>
#include <format>

namespace aaf::rpc
{

namespace
{

constexpr int kMaxDepth = 32;

auto hex(std::span<const std::byte> bytes) -> std::string
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes)
    {
        out += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return out;
}

auto unhex(std::string_view text) -> Result<std::vector<std::byte>>
{
    if (text.size() % 2 != 0)
    {
        return fail(Errc::invalid_argument, "hex string has odd length");
    }
    std::vector<std::byte> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        unsigned v = 0;
        const auto* first = text.data() + i * 2;
        if (std::from_chars(first, first + 2, v, 16).ptr != first + 2)
        {
            return fail(Errc::invalid_argument, "invalid hex string");
        }
        out[i] = static_cast<std::byte>(v);
    }
    return out;
}

template <typename T>
auto parseInteger(const Json& v) -> Result<T>
{
    if (v.is_number_integer())
    {
        return v.get<T>();
    }
    if (!v.is_string())
    {
        return fail(Errc::invalid_argument, "integer must be a string or number");
    }
    const auto text = v.get<std::string>();
    T out{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (ec != std::errc{} || ptr != text.data() + text.size())
    {
        return fail(Errc::invalid_argument, std::format("invalid integer '{}'", text));
    }
    return out;
}

auto fromJsonImpl(const Json& j, int depth) -> Result<Value>
{
    if (depth > kMaxDepth)
    {
        return fail(Errc::limit, "value nesting too deep");
    }
    if (!j.is_object() || !j.contains("t") || !j["t"].is_string())
    {
        return fail(Errc::invalid_argument, "value must be an object with a type tag \"t\"");
    }
    const auto tag = j["t"].get<std::string>();
    const auto& v = j.contains("v") ? j["v"] : Json();
    auto items = [&](const Json& list) -> Result<std::vector<Value>> {
        if (!list.is_array())
        {
            return fail(Errc::invalid_argument, "expected an array");
        }
        std::vector<Value> out;
        for (const auto& item : list)
        {
            auto x = fromJsonImpl(item, depth + 1);
            if (!x)
            {
                return std::unexpected(x.error());
            }
            out.push_back(std::move(*x));
        }
        return out;
    };
    if (tag == "null")
    {
        return Value();
    }
    if (tag == "bool" && v.is_boolean())
    {
        return Value(v.get<bool>());
    }
    if (tag == "int")
    {
        return parseInteger<std::int64_t>(v).transform([](std::int64_t x) -> Value { return x; });
    }
    if (tag == "uint")
    {
        return parseInteger<std::uint64_t>(v).transform([](std::uint64_t x) -> Value { return x; });
    }
    if (tag == "string" && v.is_string())
    {
        return Value(v.get<std::string>());
    }
    if (tag == "auid" && v.is_string())
    {
        return Auid::parse(v.get<std::string>()).transform([](const Auid& x) -> Value { return x; });
    }
    if (tag == "mobid" && v.is_string())
    {
        return MobId::parse(v.get<std::string>()).transform([](const MobId& x) -> Value { return x; });
    }
    if (tag == "enum")
    {
        Value::Enum e;
        e.name = j.value("name", std::string{});
        if (!v.is_null())
        {
            auto n = parseInteger<std::int64_t>(v);
            if (!n)
            {
                return std::unexpected(n.error());
            }
            e.value = *n;
        }
        return Value(std::move(e));
    }
    if (tag == "extenum")
    {
        Value::ExtEnum e;
        e.name = j.value("name", std::string{});
        if (v.is_string() && !v.get<std::string>().empty())
        {
            auto id = Auid::parse(v.get<std::string>());
            if (!id)
            {
                return std::unexpected(id.error());
            }
            e.value = *id;
        }
        return Value(std::move(e));
    }
    if (tag == "record" && j.contains("fields") && j["fields"].is_array())
    {
        Value::Record record;
        for (const auto& field : j["fields"])
        {
            if (!field.contains("name") || !field.contains("value"))
            {
                return fail(Errc::invalid_argument, "record field needs name and value");
            }
            auto x = fromJsonImpl(field["value"], depth + 1);
            if (!x)
            {
                return x;
            }
            record.names.push_back(field["name"].get<std::string>());
            record.values.push_back(std::move(*x));
        }
        return Value(std::move(record));
    }
    if (tag == "array" && j.contains("items"))
    {
        return items(j["items"]).transform([](std::vector<Value> x) -> Value { return x; });
    }
    if (tag == "indirect" && j.contains("type") && j.contains("value"))
    {
        auto type = Auid::parse(j["type"].get<std::string>());
        auto inner = fromJsonImpl(j["value"], depth + 1);
        if (!type || !inner)
        {
            return fail(Errc::invalid_argument, "invalid indirect value");
        }
        Value::Indirect indirect{ *type, {} };
        indirect.value.push_back(std::move(*inner));
        return Value(std::move(indirect));
    }
    if ((tag == "opaque" || tag == "bytes") && j.contains(tag == "opaque" ? "bytes" : "v"))
    {
        auto bytes = unhex(j[tag == "opaque" ? "bytes" : "v"].get<std::string>());
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }
        if (tag == "bytes")
        {
            return Value(std::move(*bytes));
        }
        auto type = Auid::parse(j.value("type", std::string{}));
        return Value(Value::Opaque{ type ? *type : Auid{}, std::move(*bytes) });
    }
    return fail(Errc::invalid_argument, std::format("invalid value with tag '{}'", tag));
}

}

auto toJson(const Value& value) -> Json
{
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return { { "t", "null" } };
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return { { "t", "bool" }, { "v", v } };
            }
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                return { { "t", "int" }, { "v", std::to_string(v) } };
            }
            else if constexpr (std::is_same_v<T, std::uint64_t>)
            {
                return { { "t", "uint" }, { "v", std::to_string(v) } };
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return { { "t", "string" }, { "v", v } };
            }
            else if constexpr (std::is_same_v<T, Auid>)
            {
                return { { "t", "auid" }, { "v", v.toString() } };
            }
            else if constexpr (std::is_same_v<T, MobId>)
            {
                return { { "t", "mobid" }, { "v", v.toString() } };
            }
            else if constexpr (std::is_same_v<T, Value::Enum>)
            {
                return { { "t", "enum" }, { "v", std::to_string(v.value) }, { "name", v.name } };
            }
            else if constexpr (std::is_same_v<T, Value::ExtEnum>)
            {
                return { { "t", "extenum" }, { "v", v.value.toString() }, { "name", v.name } };
            }
            else if constexpr (std::is_same_v<T, Value::Record>)
            {
                Json fields = Json::array();
                for (std::size_t i = 0; i < v.names.size(); ++i)
                {
                    fields.push_back({ { "name", v.names[i] }, { "value", toJson(v.values[i]) } });
                }
                return { { "t", "record" }, { "fields", std::move(fields) } };
            }
            else if constexpr (std::is_same_v<T, Value::Array>)
            {
                Json list = Json::array();
                for (const auto& item : v)
                {
                    list.push_back(toJson(item));
                }
                return { { "t", "array" }, { "items", std::move(list) } };
            }
            else if constexpr (std::is_same_v<T, Value::Indirect>)
            {
                return { { "t", "indirect" }, { "type", v.type.toString() }, { "value", v.value.empty() ? Json{ { "t", "null" } } : toJson(v.value.front()) } };
            }
            else if constexpr (std::is_same_v<T, Value::Opaque>)
            {
                return { { "t", "opaque" }, { "type", v.type.toString() }, { "bytes", hex(v.bytes) } };
            }
            else
            {
                return { { "t", "bytes" }, { "v", hex(v) } };
            }
        },
        value.data
    );
}

auto valueFromJson(const Json& json) -> Result<Value>
{
    return fromJsonImpl(json, 0);
}

auto typeToJson(const MetaModel& model, const TypeDef& type) -> Json
{
    Json j = { { "id", type.id.toString() }, { "name", type.name }, { "kind", std::string(to_string(type.kind)) } };
    if (!type.element.isNull())
    {
        j["element"] = type.element.toString();
        if (type.kind == TypeKind::strong_ref || type.kind == TypeKind::weak_ref)
        {
            const auto* cls = model.findClass(type.element);
            j["className"] = cls != nullptr ? cls->name : type.element.toString();
        }
    }
    if (type.kind == TypeKind::integer || type.kind == TypeKind::generic_character)
    {
        j["size"] = type.size;
        j["signed"] = type.isSigned;
    }
    if (type.kind == TypeKind::fixed_array)
    {
        j["count"] = type.count;
    }
    if (!type.fields.empty())
    {
        j["fields"] = Json::array();
        for (const auto& f : type.fields)
        {
            j["fields"].push_back({ { "name", f.name }, { "type", f.type.toString() } });
        }
    }
    if (!type.enumElements.empty())
    {
        j["elements"] = Json::array();
        for (const auto& e : type.enumElements)
        {
            j["elements"].push_back({ { "name", e.name }, { "value", std::to_string(e.value) } });
        }
    }
    if (!type.extElements.empty())
    {
        j["elements"] = Json::array();
        for (const auto& e : type.extElements)
        {
            j["elements"].push_back({ { "name", e.name }, { "value", e.value.toString() } });
        }
    }
    return j;
}

void collectTypes(const MetaModel& model, const Auid& typeId, Json& out, int depth)
{
    const auto key = typeId.toString();
    if (depth > kMaxDepth || out.contains(key))
    {
        return;
    }
    const auto* t = model.findType(typeId);
    if (t == nullptr)
    {
        return;
    }
    out[key] = typeToJson(model, *t);
    if (!t->element.isNull() && t->kind != TypeKind::strong_ref && t->kind != TypeKind::weak_ref)
    {
        collectTypes(model, t->element, out, depth + 1);
    }
    for (const auto& f : t->fields)
    {
        collectTypes(model, f.type, out, depth + 1);
    }
}

}
