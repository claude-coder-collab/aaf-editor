#include <aaf/edit/defaults.hpp>
#include <aaf/edit/operations.hpp>
#include <aaf/rpc/server.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <set>

namespace aaf::rpc
{

namespace
{

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kApplicationError = -32000;
constexpr std::size_t kDefaultPage = 500;
constexpr std::size_t kSearchLimit = 200;

struct RpcError
{
    int code;
    std::string message;
};

auto invalid(std::string message) -> std::unexpected<Error>
{
    return fail(Errc::invalid_argument, std::move(message));
}

template <typename T>
auto param(const Json& params, const char* key) -> Result<T>
{
    if (!params.is_object() || !params.contains(key))
    {
        return invalid(std::format("missing parameter '{}'", key));
    }
    try
    {
        return params.at(key).get<T>();
    } catch (const Json::exception&)
    {
        return invalid(std::format("parameter '{}' has the wrong type", key));
    }
}

template <typename T>
auto optionalParam(const Json& params, const char* key, T fallback) -> Result<T>
{
    if (!params.is_object() || !params.contains(key) || params.at(key).is_null())
    {
        return fallback;
    }
    return param<T>(params, key);
}

auto storedFormName(const Property& p) -> std::string_view
{
    return std::visit(
        [](const auto& payload) -> std::string_view {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, DataProperty>)
            {
                return "data";
            }
            else if constexpr (std::is_same_v<T, StrongRefProperty>)
            {
                return "strongRef";
            }
            else if constexpr (std::is_same_v<T, StrongRefVectorProperty>)
            {
                return "strongRefVector";
            }
            else if constexpr (std::is_same_v<T, StrongRefSetProperty>)
            {
                return "strongRefSet";
            }
            else if constexpr (std::is_same_v<T, WeakRefProperty>)
            {
                return "weakRef";
            }
            else if constexpr (std::is_same_v<T, WeakRefCollectionProperty>)
            {
                return "weakRefCollection";
            }
            else if constexpr (std::is_same_v<T, StreamProperty>)
            {
                return "stream";
            }
            else
            {
                return "unknown";
            }
        },
        p.payload
    );
}

auto formName(std::optional<StoredForm> form) -> std::string_view
{
    switch (form.value_or(StoredForm::data))
    {
        case StoredForm::data:
            return "data";
        case StoredForm::strongRef:
            return "strongRef";
        case StoredForm::strongRefVector:
            return "strongRefVector";
        case StoredForm::strongRefSet:
            return "strongRefSet";
        case StoredForm::weakRef:
            return "weakRef";
        case StoredForm::weakRefVector:
        case StoredForm::weakRefSet:
            return "weakRefCollection";
        case StoredForm::dataStream:
            return "stream";
    }
    return "unknown";
}

struct Child
{
    ObjectId id;
    std::uint16_t pid;
    std::size_t index;
    std::string key;
};

auto childrenOf(const Document& doc, ObjectId id) -> std::vector<Child>
{
    std::vector<Child> out;
    const auto& o = doc.object(id);
    for (const auto& p : o.properties)
    {
        if (const auto* s = std::get_if<StrongRefProperty>(&p.payload))
        {
            out.push_back({ s->object, p.pid, 0, {} });
        }
        else if (const auto* v = std::get_if<StrongRefVectorProperty>(&p.payload))
        {
            for (std::size_t i = 0; i < v->objects.size(); ++i)
            {
                out.push_back({ v->objects[i], p.pid, i, {} });
            }
        }
        else if (const auto* set = std::get_if<StrongRefSetProperty>(&p.payload))
        {
            for (std::size_t i = 0; i < set->objects.size(); ++i)
            {
                out.push_back({ set->objects[i], p.pid, i, formatKey(set->entries[i].key, o.bigEndian()) });
            }
        }
    }
    return out;
}

auto childCount(const Document& doc, ObjectId id) -> std::size_t
{
    std::size_t n = 0;
    for (const auto& p : doc.object(id).properties)
    {
        if (std::holds_alternative<StrongRefProperty>(p.payload))
        {
            ++n;
        }
        else if (const auto* v = std::get_if<StrongRefVectorProperty>(&p.payload))
        {
            n += v->objects.size();
        }
        else if (const auto* s = std::get_if<StrongRefSetProperty>(&p.payload))
        {
            n += s->objects.size();
        }
    }
    return n;
}

auto className(const Document& doc, ObjectId id) -> std::string
{
    const auto* cls = doc.classOf(id);
    return cls != nullptr ? cls->name : doc.object(id).classId.toString();
}

auto propertyName(const Document& doc, std::uint16_t pid) -> std::string
{
    const auto* def = doc.model().findPropertyByPid(pid);
    return def != nullptr ? def->name : std::format("{:#06x}", pid);
}

auto weakTarget(const Document& doc, const Object& o, std::uint16_t tag, std::span<const std::byte> key) -> Json
{
    Json j = { { "key", formatKey(key, o.bigEndian()) } };
    if (const auto target = doc.resolveWeak(tag, key))
    {
        j["id"] = *target;
        j["label"] = labelOf(doc, *target);
    }
    else
    {
        j["id"] = nullptr;
        j["label"] = formatKey(key, o.bigEndian());
        j["resolved"] = isKnownDefinitionKey(doc.model(), key, o.bigEndian()) ? "builtin" : "missing";
    }
    return j;
}

auto propertyJson(const Document& doc, const Object& o, const Property& p, Json& types) -> Json
{
    const auto* def = doc.propertyDef(p);
    Json j = { { "pid", p.pid }, { "name", propertyName(doc, p.pid) }, { "kind", std::string(storedFormName(p)) }, { "storedForm", p.storedForm } };
    if (def != nullptr)
    {
        j["type"] = def->type.toString();
        j["optional"] = def->optional;
        j["uniqueId"] = def->uniqueId;
        collectTypes(doc.model(), def->type, types);
    }
    std::visit(
        [&](const auto& payload) -> auto {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, DataProperty>)
            {
                if (auto v = doc.decode(o, p))
                {
                    j["value"] = toJson(*v);
                }
                else
                {
                    j["error"] = v.error().message;
                    j["value"] = toJson(Value(payload.bytes));
                }
            }
            else if constexpr (std::is_same_v<T, StrongRefProperty>)
            {
                j["children"] = Json::array({ { { "id", payload.object }, { "class", className(doc, payload.object) }, { "label", labelOf(doc, payload.object) } } });
            }
            else if constexpr (std::is_same_v<T, StrongRefVectorProperty> || std::is_same_v<T, StrongRefSetProperty>)
            {
                j["count"] = payload.objects.size();
            }
            else if constexpr (std::is_same_v<T, WeakRefProperty>)
            {
                j["target"] = weakTarget(doc, o, payload.tag, payload.key);
            }
            else if constexpr (std::is_same_v<T, WeakRefCollectionProperty>)
            {
                j["targets"] = Json::array();
                for (const auto& key : payload.keys)
                {
                    j["targets"].push_back(weakTarget(doc, o, payload.tag, key));
                }
            }
            else if constexpr (std::is_same_v<T, StreamProperty>)
            {
                j["size"] = payload.size;
            }
            else
            {
                j["value"] = toJson(Value(payload.bytes));
            }
        },
        p.payload
    );
    return j;
}

auto objectJson(const Document& doc, ObjectId id) -> Json
{
    const auto& o = doc.object(id);
    Json types = Json::object();
    Json properties = Json::array();
    for (const auto& p : o.properties)
    {
        properties.push_back(propertyJson(doc, o, p, types));
    }
    Json available = Json::array();
    for (const auto* def : doc.model().allProperties(o.classId))
    {
        if (def->pid == 0 || def->pid == ids::kPidObjClass || o.find(def->pid) != nullptr)
        {
            continue;
        }
        available.push_back({ { "pid", def->pid }, { "name", def->name }, { "type", def->type.toString() }, { "optional", def->optional }, { "kind", std::string(formName(expectedStoredForm(doc.model(), def->type))) } });
        collectTypes(doc.model(), def->type, types);
    }
    const auto* cls = doc.classOf(id);
    return {
        { "id", id },
        { "class", className(doc, id) },
        { "classId", o.classId.toString() },
        { "concrete", cls != nullptr && cls->concrete },
        { "label", labelOf(doc, id) },
        { "parent", o.parent == kNoObject ? Json(nullptr) : Json(o.parent) },
        { "parentPid", o.parentPid },
        { "attached", doc.isAttached(id) },
        { "properties", std::move(properties) },
        { "available", std::move(available) },
        { "types", std::move(types) },
    };
}

auto diagnosticJson(const Document& doc, const Diagnostic& d) -> Json
{
    return {
        { "severity", std::string(to_string(d.severity)) },
        { "object", d.object == kNoObject ? Json(nullptr) : Json(d.object) },
        { "pid", d.pid },
        { "property", d.pid == 0 ? std::string{} : propertyName(doc, d.pid) },
        { "message", d.message },
    };
}

auto resolveClass(const MetaModel& model, const std::string& name) -> Result<Auid>
{
    if (const auto* cls = model.findClassByName(name))
    {
        return cls->id;
    }
    if (auto id = Auid::parse(name); id && model.findClass(*id) != nullptr)
    {
        return *id;
    }
    return fail(Errc::not_found, std::format("unknown class '{}'", name));
}

auto lower(std::string_view text) -> std::string
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
    return out;
}

auto matches(const Document& doc, ObjectId id, const std::string& needle) -> bool
{
    if (lower(labelOf(doc, id)).contains(needle))
    {
        return true;
    }
    const auto& o = doc.object(id);
    for (const auto& p : o.properties)
    {
        if (!std::holds_alternative<DataProperty>(p.payload))
        {
            continue;
        }
        auto v = doc.decode(o, p);
        if (!v)
        {
            continue;
        }
        std::string text;
        if (v->is<std::string>())
        {
            text = v->as<std::string>();
        }
        else if (v->is<Auid>() || v->is<MobId>())
        {
            text = v->toString();
        }
        if (!text.empty() && lower(text).contains(needle))
        {
            return true;
        }
    }
    return false;
}

}

auto labelOf(const Document& document, ObjectId id) -> std::string
{
    const auto& o = document.object(id);
    const auto& model = document.model();
    const PropertyDef* unique = nullptr;
    for (const auto* def : model.allProperties(o.classId))
    {
        if (def->name == "Name" && def->pid != 0)
        {
            if (const auto* p = o.find(def->pid))
            {
                if (auto v = document.decode(o, *p); v && v->is<std::string>() && !v->as<std::string>().empty())
                {
                    return v->as<std::string>();
                }
            }
        }
        if (def->uniqueId && def->pid != 0 && unique == nullptr)
        {
            unique = def;
        }
    }
    if (unique != nullptr)
    {
        if (const auto* p = o.find(unique->pid))
        {
            if (auto v = document.decode(o, *p))
            {
                return v->toString();
            }
        }
    }
    return className(document, id);
}

auto changeSetToJson(const edit::ChangeSet& changes) -> Json
{
    Json properties = Json::array();
    for (const auto& p : changes.properties)
    {
        properties.push_back({ { "object", p.object }, { "pid", p.pid } });
    }
    return { { "objects", changes.objects }, { "created", changes.created }, { "properties", std::move(properties) }, { "referencedPropertiesChanged", changes.referencedPropertiesChanged } };
}

void Server::emit(const std::string& method, const Json& params) const
{
    if (sink_)
    {
        sink_(method, params);
    }
}

auto Server::requireSession() -> Result<edit::Session*>
{
    if (!session_)
    {
        return fail(Errc::invalid_argument, "no document is open");
    }
    return &*session_;
}

auto Server::info() const -> Json
{
    if (!session_)
    {
        return { { "open", false } };
    }
    const auto& doc = session_->document();
    const auto history = session_->history();
    const auto position = session_->position();
    return {
        { "open", true },
        { "path", path_.string() },
        { "name", path_.filename().string() },
        { "dirty", session_->dirty() },
        { "canUndo", session_->canUndo() },
        { "canRedo", session_->canRedo() },
        { "undo", position > 0 ? Json(history[position - 1]) : Json(nullptr) },
        { "redo", position < history.size() ? Json(history[position]) : Json(nullptr) },
        { "objectCount", doc.objectCount() },
        { "version", doc.container().header().version == cfb::Version::v3 ? 3 : 4 },
        { "root", Document::root() },
        { "header", doc.header() == kNoObject ? Json(nullptr) : Json(doc.header()) },
        { "metaDictionary", doc.metaDictionary() == kNoObject ? Json(nullptr) : Json(doc.metaDictionary()) },
    };
}

void Server::attachListener()
{
    if (!session_)
    {
        return;
    }
    session_->setListener([this](const edit::ChangeSet& changes) -> void {
        emit("doc.changed", { { "changes", changeSetToJson(changes) }, { "info", info() } });
    });
}

auto Server::run(const std::string& description, const edit::Session::Command& command) -> Result<Json>
{
    auto session = requireSession();
    if (!session)
    {
        return std::unexpected(session.error());
    }
    auto changes = (*session)->execute(description, command);
    if (!changes)
    {
        return std::unexpected(changes.error());
    }
    return changeSetToJson(*changes);
}

auto Server::call(const std::string& method, const Json& params) -> Result<Json>
{
    static const std::set<std::string, std::less<>> kMethods = {
        "doc.info",
        "doc.open",
        "doc.close",
        "doc.save",
        "doc.saveAs",
        "doc.validate",
        "tree.children",
        "tree.path",
        "object.get",
        "object.setProperty",
        "object.removeProperty",
        "object.create",
        "object.delete",
        "object.move",
        "object.setWeakRef",
        "object.candidates",
        "model.subclasses",
        "edit.undo",
        "edit.redo",
        "edit.history",
        "search.query",
        "essence.extract",
        "essence.replace",
    };
    if (!kMethods.contains(method))
    {
        return fail(Errc::not_found, std::format("unknown method '{}'", method));
    }
    if (method == "doc.info")
    {
        return info();
    }
    if (method == "doc.open")
    {
        auto path = param<std::string>(params, "path");
        if (!path)
        {
            return std::unexpected(path.error());
        }
        auto opened = edit::Session::open(*path);
        if (!opened)
        {
            return std::unexpected(opened.error());
        }
        session_.reset();
        session_.emplace(std::move(*opened));
        path_ = *path;
        attachListener();
        emit("doc.opened", info());
        return info();
    }
    if (method == "doc.close")
    {
        session_.reset();
        path_.clear();
        emit("doc.opened", info());
        return info();
    }

    auto session = requireSession();
    if (!session)
    {
        return std::unexpected(session.error());
    }
    auto& s = **session;
    const auto& doc = s.document();
    auto objectParam = [&](const char* key) -> Result<ObjectId> {
        auto id = param<ObjectId>(params, key);
        if (id && *id >= doc.objectCount())
        {
            return invalid(std::format("object {} does not exist", *id));
        }
        return id;
    };

    if (method == "doc.save" || method == "doc.saveAs")
    {
        auto target = method == "doc.save" ? Result<std::string>(path_.string()) : param<std::string>(params, "path");
        auto regenerate = optionalParam<bool>(params, "regenerateLayout", false);
        auto version = optionalParam<int>(params, "version", 0);
        if (!target || !regenerate || !version)
        {
            return invalid("invalid save parameters");
        }
        WriteOptions options{ .preserveLayout = !*regenerate, .version = std::nullopt };
        if (*version == 3 || *version == 4)
        {
            options.version = *version == 3 ? cfb::Version::v3 : cfb::Version::v4;
        }
        if (auto r = s.save(*target, options); !r)
        {
            return std::unexpected(r.error());
        }
        path_ = *target;
        emit("doc.state", info());
        return info();
    }
    if (method == "doc.validate")
    {
        Json out = Json::array();
        for (const auto& d : validate(doc))
        {
            out.push_back(diagnosticJson(doc, d));
        }
        return out;
    }
    if (method == "tree.children")
    {
        auto id = objectParam("id");
        auto offset = optionalParam<std::size_t>(params, "offset", 0);
        auto limit = optionalParam<std::size_t>(params, "limit", kDefaultPage);
        if (!id || !offset || !limit)
        {
            return invalid("invalid tree.children parameters");
        }
        const auto all = childrenOf(doc, *id);
        Json items = Json::array();
        for (std::size_t i = *offset; i < all.size() && i < *offset + *limit; ++i)
        {
            const auto& c = all[i];
            items.push_back({ { "id", c.id }, { "class", className(doc, c.id) }, { "label", labelOf(doc, c.id) }, { "pid", c.pid }, { "property", propertyName(doc, c.pid) }, { "index", c.index }, { "key", c.key }, { "childCount", childCount(doc, c.id) } });
        }
        return Json{ { "total", all.size() }, { "items", std::move(items) } };
    }
    if (method == "tree.path")
    {
        auto id = objectParam("id");
        if (!id)
        {
            return std::unexpected(id.error());
        }
        Json path = Json::array();
        for (ObjectId a = *id; a != kNoObject; a = doc.object(a).parent)
        {
            path.insert(path.begin(), a);
            if (a == Document::root())
            {
                break;
            }
        }
        return path;
    }
    if (method == "object.get")
    {
        auto id = objectParam("id");
        if (!id)
        {
            return std::unexpected(id.error());
        }
        return objectJson(doc, *id);
    }
    if (method == "object.setProperty")
    {
        auto id = objectParam("id");
        auto pid = param<std::uint16_t>(params, "pid");
        if (!id || !pid || !params.contains("value"))
        {
            return invalid("object.setProperty needs id, pid and value");
        }
        auto value = valueFromJson(params["value"]);
        if (!value)
        {
            return std::unexpected(value.error());
        }
        return run(std::format("Set {}", propertyName(doc, *pid)), [&](edit::Transaction& tx) -> Result<void> { return edit::setProperty(tx, *id, *pid, *value); });
    }
    if (method == "object.removeProperty")
    {
        auto id = objectParam("id");
        auto pid = param<std::uint16_t>(params, "pid");
        if (!id || !pid)
        {
            return invalid("object.removeProperty needs id and pid");
        }
        return run(std::format("Remove {}", propertyName(doc, *pid)), [&](edit::Transaction& tx) -> Result<void> { return edit::removeProperty(tx, *id, *pid); });
    }
    if (method == "object.create")
    {
        auto parent = objectParam("parent");
        auto pid = param<std::uint16_t>(params, "pid");
        auto cls = param<std::string>(params, "class");
        auto index = optionalParam<std::size_t>(params, "index", std::numeric_limits<std::size_t>::max());
        if (!parent || !pid || !cls || !index)
        {
            return invalid("object.create needs parent, pid and class");
        }
        auto classId = resolveClass(doc.model(), *cls);
        if (!classId)
        {
            return std::unexpected(classId.error());
        }
        ObjectId created = kNoObject;
        auto changes = run(std::format("Add {}", *cls), [&](edit::Transaction& tx) -> Result<void> {
            auto child = edit::createWithDefaults(tx, *classId);
            if (!child)
            {
                return std::unexpected(child.error());
            }
            created = *child;
            const auto* def = doc.model().findPropertyByPid(*pid);
            const auto form = def == nullptr ? std::nullopt : expectedStoredForm(doc.model(), def->type);
            if (form == StoredForm::strongRef)
            {
                return edit::setStrongRef(tx, *parent, *pid, created);
            }
            if (auto r = edit::ensureCollection(tx, *parent, *pid); !r)
            {
                return r;
            }
            const auto* p = doc.object(*parent).find(*pid);
            const auto* v = p == nullptr ? nullptr : std::get_if<StrongRefVectorProperty>(&p->payload);
            const auto size = v == nullptr ? std::size_t{ 0 } : v->objects.size();
            return edit::insertIntoCollection(tx, *parent, *pid, std::min(*index, size), created);
        });
        if (!changes)
        {
            return changes;
        }
        return Json{ { "id", created }, { "changes", std::move(*changes) } };
    }
    if (method == "object.delete")
    {
        auto id = objectParam("id");
        auto force = optionalParam<bool>(params, "force", false);
        if (!id || !force)
        {
            return invalid("object.delete needs id");
        }
        return run(std::format("Delete {}", className(doc, *id)), [&](edit::Transaction& tx) -> Result<void> { return edit::deleteObject(tx, *id, *force); });
    }
    if (method == "object.move")
    {
        auto parent = objectParam("parent");
        auto pid = param<std::uint16_t>(params, "pid");
        auto from = param<std::size_t>(params, "from");
        auto to = param<std::size_t>(params, "to");
        if (!parent || !pid || !from || !to)
        {
            return invalid("object.move needs parent, pid, from and to");
        }
        return run("Reorder", [&](edit::Transaction& tx) -> Result<void> { return edit::moveInCollection(tx, *parent, *pid, *from, *to); });
    }
    if (method == "object.setWeakRef")
    {
        auto id = objectParam("id");
        auto pid = param<std::uint16_t>(params, "pid");
        auto target = objectParam("target");
        if (!id || !pid || !target)
        {
            return invalid("object.setWeakRef needs id, pid and target");
        }
        return run(std::format("Set {}", propertyName(doc, *pid)), [&](edit::Transaction& tx) -> Result<void> { return edit::setWeakRef(tx, *id, *pid, *target); });
    }
    if (method == "object.candidates")
    {
        auto id = objectParam("id");
        auto pid = param<std::uint16_t>(params, "pid");
        if (!id || !pid)
        {
            return invalid("object.candidates needs id and pid");
        }
        Json out = Json::array();
        for (const auto c : edit::weakCandidates(doc, *id, *pid))
        {
            out.push_back({ { "id", c }, { "class", className(doc, c) }, { "label", labelOf(doc, c) } });
        }
        return out;
    }
    if (method == "model.subclasses")
    {
        auto cls = param<std::string>(params, "class");
        if (!cls)
        {
            return std::unexpected(cls.error());
        }
        auto base = resolveClass(doc.model(), *cls);
        if (!base)
        {
            return std::unexpected(base.error());
        }
        std::vector<std::pair<std::string, std::string>> found;
        for (const auto& [id, c] : doc.model().classes())
        {
            if (c.concrete && doc.model().isA(id, *base))
            {
                found.emplace_back(c.name, id.toString());
            }
        }
        std::ranges::sort(found);
        Json out = Json::array();
        for (const auto& [name, id] : found)
        {
            out.push_back({ { "name", name }, { "id", id } });
        }
        return out;
    }
    if (method == "edit.undo" || method == "edit.redo")
    {
        auto changes = method == "edit.undo" ? s.undo() : s.redo();
        if (!changes)
        {
            return std::unexpected(changes.error());
        }
        return changeSetToJson(*changes);
    }
    if (method == "edit.history")
    {
        return Json{ { "items", s.history() }, { "position", s.position() } };
    }
    if (method == "search.query")
    {
        auto text = optionalParam<std::string>(params, "text", {});
        auto cls = optionalParam<std::string>(params, "class", {});
        auto limit = optionalParam<std::size_t>(params, "limit", kSearchLimit);
        if (!text || !cls || !limit)
        {
            return invalid("invalid search parameters");
        }
        std::optional<Auid> classId;
        if (!cls->empty())
        {
            auto resolved = resolveClass(doc.model(), *cls);
            if (!resolved)
            {
                return std::unexpected(resolved.error());
            }
            classId = *resolved;
        }
        const auto needle = lower(*text);
        Json out = Json::array();
        for (std::size_t i = 0; i < doc.objectCount() && out.size() < *limit; ++i)
        {
            if (!doc.isAttached(i) || (classId && !doc.model().isA(doc.object(i).classId, *classId)) || (!needle.empty() && !matches(doc, i, needle)))
            {
                continue;
            }
            out.push_back({ { "id", i }, { "class", className(doc, i) }, { "label", labelOf(doc, i) } });
        }
        return out;
    }
    if (method == "essence.extract" || method == "essence.replace")
    {
        auto id = objectParam("id");
        auto path = param<std::string>(params, "path");
        if (!id || !path)
        {
            return invalid(std::format("{} needs id and path", method));
        }
        const auto* def = doc.model().findProperty("EssenceData", "Data");
        const auto* p = def == nullptr ? nullptr : doc.object(*id).find(def->pid);
        const auto* stream = p == nullptr ? nullptr : std::get_if<StreamProperty>(&p->payload);
        if (stream == nullptr)
        {
            return invalid("object has no essence data");
        }
        if (method == "essence.extract")
        {
            std::uint64_t written = 0;
            auto r = cfb::writeFileAtomic(*path, [&](cfb::ByteSink& sink) -> Result<void> {
                auto n = copyStream(doc, *stream, sink);
                if (!n)
                {
                    return std::unexpected(n.error());
                }
                written = *n;
                return {};
            });
            if (!r)
            {
                return std::unexpected(r.error());
            }
            return Json{ { "size", written } };
        }
        auto source = cfb::FileSource::open(*path);
        if (!source)
        {
            return std::unexpected(source.error());
        }
        std::shared_ptr<const cfb::ByteSource> shared = std::move(*source);
        return run("Replace essence", [&](edit::Transaction& tx) -> Result<void> { return edit::setStreamData(tx, *id, def->pid, shared); });
    }
    return fail(Errc::not_found, std::format("unknown method '{}'", method));
}

auto Server::handle(std::string_view request) -> std::string
{
    Json id = nullptr;
    auto respondError = [&](int code, const std::string& message) -> std::string {
        return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } }.dump();
    };
    Json parsed;
    try
    {
        parsed = Json::parse(request);
    } catch (const Json::parse_error& e)
    {
        return respondError(kParseError, e.what());
    }
    if (!parsed.is_object() || parsed.value("jsonrpc", "") != "2.0" || !parsed.contains("method") || !parsed["method"].is_string())
    {
        return respondError(kInvalidRequest, "invalid JSON-RPC 2.0 request");
    }
    const bool notification = !parsed.contains("id");
    id = parsed.value("id", Json());
    const auto method = parsed["method"].get<std::string>();
    const auto params = parsed.value("params", Json::object());
    Result<Json> result = fail(Errc::io, "internal error");
    try
    {
        result = call(method, params);
    } catch (const std::exception& e)
    {
        result = fail(Errc::io, e.what());
    }
    if (notification)
    {
        return {};
    }
    if (!result)
    {
        const auto& error = result.error();
        int code = kApplicationError;
        if (error.code == Errc::not_found && error.message.starts_with("unknown method"))
        {
            code = kMethodNotFound;
        }
        else if (error.code == Errc::invalid_argument && error.message.starts_with("missing parameter"))
        {
            code = kInvalidParams;
        }
        return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", error.message }, { "data", { { "kind", std::string(to_string(error.code)) } } } } } }.dump();
    }
    return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(*result) } }.dump();
}

}
