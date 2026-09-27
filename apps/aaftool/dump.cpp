#include "dump.hpp"

#include <format>

namespace aaftool
{

namespace
{

using nlohmann::ordered_json;

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

auto className(const aaf::Document& doc, aaf::ObjectId id) -> std::string
{
    const auto* cls = doc.classOf(id);
    return cls != nullptr ? cls->name : doc.object(id).classId.toString();
}

auto propertyName(const aaf::Document& doc, const aaf::Property& p) -> std::string
{
    const auto* def = doc.propertyDef(p);
    return def != nullptr ? def->name : std::format("{:#06x}", p.pid);
}

auto propertyJson(const aaf::Document& doc, const aaf::Object& o, const aaf::Property& p, int depth, int maxDepth) -> ordered_json
{
    auto child = [&](aaf::ObjectId id) -> nlohmann::ordered_json { return depth < maxDepth ? toJson(doc, id, maxDepth - depth - 1) : ordered_json(className(doc, id)); };
    auto children = [&](const std::vector<aaf::ObjectId>& ids) -> ordered_json {
        ordered_json list = ordered_json::array();
        for (const auto id : ids)
        {
            list.push_back(child(id));
        }
        return list;
    };
    return std::visit(
        [&](const auto& payload) -> ordered_json {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, aaf::DataProperty>)
            {
                auto v = doc.decode(o, p);
                if (!v)
                {
                    return { { "error", v.error().message }, { "bytes", hex(payload.bytes) } };
                }
                return toJson(*v);
            }
            else if constexpr (std::is_same_v<T, aaf::StrongRefProperty>)
            {
                return child(payload.object);
            }
            else if constexpr (std::is_same_v<T, aaf::StrongRefVectorProperty> || std::is_same_v<T, aaf::StrongRefSetProperty>)
            {
                return children(payload.objects);
            }
            else if constexpr (std::is_same_v<T, aaf::WeakRefProperty>)
            {
                return aaf::formatKey(payload.key, o.bigEndian());
            }
            else if constexpr (std::is_same_v<T, aaf::WeakRefCollectionProperty>)
            {
                ordered_json list = ordered_json::array();
                for (const auto& key : payload.keys)
                {
                    list.push_back(aaf::formatKey(key, o.bigEndian()));
                }
                return list;
            }
            else if constexpr (std::is_same_v<T, aaf::StreamProperty>)
            {
                return { { "stream", aaf::cfb::toUtf8(payload.name) }, { "size", payload.size } };
            }
            else
            {
                return { { "storedForm", p.storedForm }, { "bytes", hex(payload.bytes) } };
            }
        },
        p.payload
    );
}

void appendText(const aaf::Document& doc, aaf::ObjectId id, int depth, int maxDepth, std::string& out)
{
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    const auto& o = doc.object(id);
    for (const auto& p : o.properties)
    {
        const auto name = propertyName(doc, p);
        auto nested = [&](aaf::ObjectId child, const std::string& label) -> void {
            out += std::format("{}  {}{}\n", indent, label, className(doc, child));
            if (depth < maxDepth)
            {
                appendText(doc, child, depth + 2, maxDepth, out);
            }
        };
        if (const auto* s = std::get_if<aaf::StrongRefProperty>(&p.payload))
        {
            out += std::format("{}{}:\n", indent, name);
            nested(s->object, "");
        }
        else if (const auto* v = std::get_if<aaf::StrongRefVectorProperty>(&p.payload))
        {
            out += std::format("{}{}: [{}]\n", indent, name, v->objects.size());
            for (std::size_t i = 0; i < v->objects.size(); ++i)
            {
                nested(v->objects[i], std::format("[{}] ", i));
            }
        }
        else if (const auto* set = std::get_if<aaf::StrongRefSetProperty>(&p.payload))
        {
            out += std::format("{}{}: {{{}}}\n", indent, name, set->objects.size());
            for (std::size_t i = 0; i < set->objects.size(); ++i)
            {
                nested(set->objects[i], std::format("[{}] ", aaf::formatKey(set->entries[i].key, o.bigEndian())));
            }
        }
        else
        {
            out += std::format("{}{} = {}\n", indent, name, propertyJson(doc, o, p, 0, 0).dump());
        }
    }
}

}

auto toJson(const aaf::Value& value) -> ordered_json
{
    return std::visit(
        [](const auto& v) -> ordered_json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return nullptr;
            }
            else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::string>)
            {
                return v;
            }
            else if constexpr (std::is_same_v<T, aaf::Auid> || std::is_same_v<T, aaf::MobId>)
            {
                return v.toString();
            }
            else if constexpr (std::is_same_v<T, aaf::Value::Enum>)
            {
                return v.name.empty() ? ordered_json(v.value) : ordered_json(v.name);
            }
            else if constexpr (std::is_same_v<T, aaf::Value::ExtEnum>)
            {
                return v.name.empty() ? v.value.toString() : v.name;
            }
            else if constexpr (std::is_same_v<T, aaf::Value::Record>)
            {
                ordered_json object = ordered_json::object();
                for (std::size_t i = 0; i < v.names.size(); ++i)
                {
                    object[v.names[i]] = toJson(v.values[i]);
                }
                return object;
            }
            else if constexpr (std::is_same_v<T, aaf::Value::Array>)
            {
                ordered_json list = ordered_json::array();
                for (const auto& item : v)
                {
                    list.push_back(toJson(item));
                }
                return list;
            }
            else if constexpr (std::is_same_v<T, aaf::Value::Indirect>)
            {
                return v.value.empty() ? ordered_json(nullptr) : toJson(v.value.front());
            }
            else if constexpr (std::is_same_v<T, aaf::Value::Opaque>)
            {
                return { { "opaque", v.type.toString() }, { "bytes", hex(v.bytes) } };
            }
            else
            {
                return hex(v);
            }
        },
        value.data
    );
}

auto toJson(const aaf::Document& doc, aaf::ObjectId id, int maxDepth) -> ordered_json
{
    const auto& o = doc.object(id);
    ordered_json properties = ordered_json::object();
    for (const auto& p : o.properties)
    {
        properties[propertyName(doc, p)] = propertyJson(doc, o, p, 0, maxDepth);
    }
    return { { "class", className(doc, id) }, { "properties", std::move(properties) } };
}

auto toText(const aaf::Document& doc, aaf::ObjectId id, int maxDepth) -> std::string
{
    std::string out = className(doc, id) + "\n";
    appendText(doc, id, 0, maxDepth == kUnlimitedDepth ? kUnlimitedDepth : maxDepth * 2, out);
    return out;
}

}
