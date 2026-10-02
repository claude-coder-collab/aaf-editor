#include <aaf/core/labels.hpp>
#include <aaf/edit/defaults.hpp>
#include <aaf/edit/operations.hpp>
#include <aaf/rpc/json_writer.hpp>
#include <aaf/rpc/server.hpp>
#include <aaf/timeline/edit.hpp>
#include <aaf/timeline/timeline.hpp>

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

constexpr std::size_t kReferencedByLimit = 50;

auto referenceJson(const Document& doc, const edit::ReferenceResolution& r) -> Json
{
    Json j = { { "status", std::string(edit::to_string(r.status)) } };
    if (r.status == edit::ReferenceStatus::resolved)
    {
        j["id"] = r.target;
        j["label"] = labelOf(doc, r.target);
        j["class"] = className(doc, r.target);
    }
    else
    {
        j["id"] = nullptr;
        if (r.status == edit::ReferenceStatus::builtin)
        {
            j["label"] = r.builtinName;
        }
    }
    return j;
}

auto propertyJson(const Document& doc, ObjectId id, const Property& p, Json& types, const edit::ReferenceIndex& references) -> Json
{
    const auto& o = doc.object(id);
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
                    if (const auto r = references.resolve(id, p.pid))
                    {
                        j["refers"] = referenceJson(doc, *r);
                    }
                    if (const auto n = references.referrersByKey(id, p.pid).size(); n > 0)
                    {
                        j["referrers"] = n;
                    }
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

auto objectJson(const Document& doc, ObjectId id, const edit::ReferenceIndex& references) -> Json
{
    const auto& o = doc.object(id);
    Json types = Json::object();
    Json properties = Json::array();
    for (const auto& p : o.properties)
    {
        properties.push_back(propertyJson(doc, id, p, types, references));
    }
    const auto incoming = references.referrers(id);
    Json referencedBy = Json::array();
    for (const auto& r : incoming.first(std::min(incoming.size(), kReferencedByLimit)))
    {
        referencedBy.push_back({ { "id", r.object }, { "class", className(doc, r.object) }, { "label", labelOf(doc, r.object) }, { "pid", r.pid }, { "property", propertyName(doc, r.pid) }, { "weak", r.weak } });
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
        { "referencedBy", std::move(referencedBy) },
        { "referencedByCount", incoming.size() },
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
            if (const auto* label = v->is<Auid>() ? findSmpteLabel(v->as<Auid>()) : nullptr)
            {
                text += ' ';
                text += label->name;
            }
        }
        if (!text.empty() && lower(text).contains(needle))
        {
            return true;
        }
    }
    return false;
}

class TimelineWriter
{
public:
    auto write(const timeline::MobTimeline& t) -> std::string
    {
        for (const auto& track : t.tracks)
        {
            collect(track.items);
        }
        w_.beginObject();
        w_.key("mob").value(t.mob);
        w_.key("mobId").value(t.mobId.toString());
        w_.key("name").value(t.name);
        w_.key("kind").value(timeline::to_string(t.kind));
        w_.key("partial").value(t.partial);
        w_.key("slots").beginArray();
        for (const auto slot : t.slots)
        {
            w_.value(slot);
        }
        w_.endArray();
        strings("warnings", t.warnings);
        w_.key("timecode");
        timecode(t.timecode);
        w_.key("sources").beginArray();
        for (const auto* source : sources_)
        {
            w_.beginObject();
            w_.key("mobId").value(source->mobId.toString());
            w_.key("mob");
            source->mob ? w_.value(*source->mob) : w_.null();
            w_.key("mobName").value(source->mobName);
            w_.key("mobKind").value(timeline::to_string(source->mobKind));
            w_.key("original").value(source->original);
            w_.endObject();
        }
        w_.endArray();
        w_.key("tracks").beginArray();
        for (const auto& track : t.tracks)
        {
            writeTrack(track);
        }
        w_.endArray();
        w_.endObject();
        return w_.take();
    }

private:
    void collect(const std::vector<timeline::Item>& items)
    {
        for (const auto& item : items)
        {
            if (item.source && refs_.try_emplace(item.source->mobId, sources_.size()).second)
            {
                sources_.push_back(&*item.source);
            }
            for (const auto& nested : item.nested)
            {
                collect(nested);
            }
        }
    }

    void strings(std::string_view name, const std::vector<std::string>& list)
    {
        w_.key(name).beginArray();
        for (const auto& text : list)
        {
            w_.value(text);
        }
        w_.endArray();
    }

    void timecode(const std::optional<timeline::Timecode>& tc)
    {
        if (!tc)
        {
            w_.null();
            return;
        }
        w_.beginObject().key("start").value(tc->start).key("fps").value(tc->fps).key("drop").value(tc->drop).endObject();
    }

    void writeTrack(const timeline::Track& track)
    {
        w_.beginObject();
        w_.key("slot").value(track.slot);
        w_.key("slotId").value(track.slotId);
        w_.key("name").value(track.name);
        w_.key("physicalNumber");
        track.physicalNumber ? w_.value(*track.physicalNumber) : w_.null();
        w_.key("kind").value(timeline::to_string(track.kind));
        w_.key("slotKind").value(timeline::to_string(track.slotKind));
        w_.key("editRate").beginObject().key("num").value(track.editRate.numerator()).key("den").value(track.editRate.denominator()).endObject();
        w_.key("origin").value(track.origin);
        w_.key("length").value(track.length);
        w_.key("segment").value(track.segment);
        w_.key("effects").beginArray();
        for (const auto& effect : track.effects)
        {
            w_.beginObject().key("object").value(effect.object).key("name").value(effect.name).endObject();
        }
        w_.endArray();
        if (track.channels != 0)
        {
            w_.key("channels").value(track.channels);
        }
        items(track.items);
        strings("warnings", track.warnings);
        w_.endObject();
    }

    void items(const std::vector<timeline::Item>& list)
    {
        w_.key("items").beginArray();
        for (const auto& item : list)
        {
            writeItem(item);
        }
        w_.endArray();
    }

    void writeItem(const timeline::Item& item)
    {
        const auto kind = timeline::to_string(item.kind);
        w_.beginObject();
        w_.key("object").value(item.object);
        w_.key("kind").value(kind);
        w_.key("start").value(item.start);
        w_.key("length").value(item.length);
        if (item.className.size() != kind.size() || item.className.empty() || std::toupper(static_cast<unsigned char>(kind.front())) != item.className.front() || std::string_view(item.className).substr(1) != kind.substr(1))
        {
            w_.key("class").value(item.className);
        }
        if (!item.hasLength)
        {
            w_.key("hasLength").value(false);
        }
        if (!item.source || item.label != item.source->mobName)
        {
            w_.key("label").value(item.label);
        }
        if (item.source)
        {
            w_.key("source").beginObject().key("ref").value(refs_.at(item.source->mobId)).key("slotId").value(item.source->slotId).key("startTime").value(item.source->startTime).endObject();
        }
        if (!item.effect.empty())
        {
            w_.key("effect").value(item.effect);
        }
        if (!item.comment.empty())
        {
            w_.key("comment").value(item.comment);
        }
        if (item.timecode)
        {
            w_.key("timecode");
            timecode(item.timecode);
        }
        if (item.clip)
        {
            w_.key("clip").value(*item.clip);
            w_.key("effects").beginArray();
            for (const auto& effect : item.effects)
            {
                w_.beginObject().key("object").value(effect.object).key("name").value(effect.name).endObject();
            }
            w_.endArray();
        }
        if (!item.channels.empty())
        {
            w_.key("channels").value(item.channels.size());
        }
        if (!item.nested.empty())
        {
            w_.key("nested").beginArray();
            for (const auto& nested : item.nested)
            {
                w_.beginArray();
                for (const auto& child : nested)
                {
                    writeItem(child);
                }
                w_.endArray();
            }
            w_.endArray();
        }
        w_.endObject();
    }

    JsonWriter w_;
    std::map<MobId, std::size_t> refs_;
    std::vector<const timeline::SourceReference*> sources_;
};

}

auto labelOf(const Document& document, ObjectId id) -> std::string
{
    const auto& o = document.object(id);
    const auto& model = document.model();
    const auto text = [&](std::string_view cls, std::string_view property) -> std::string {
        const auto v = document.value(id, cls, property);
        return v && v->is<std::string>() ? v->as<std::string>() : std::string{};
    };
    const auto* slot = model.findClassByName("MobSlot");
    if (slot != nullptr && model.isA(o.classId, slot->id))
    {
        const auto slotId = document.value(id, "MobSlot", "SlotID");
        const auto number = slotId ? std::format("Slot {}", slotId->toString()) : std::string("Slot");
        const auto name = text("MobSlot", "SlotName");
        return name.empty() ? number : std::format("{} ({})", name, number);
    }
    const auto* identification = model.findClassByName("Identification");
    if (identification != nullptr && model.isA(o.classId, identification->id))
    {
        const auto product = text("Identification", "ProductName");
        const auto version = text("Identification", "ProductVersionString");
        if (!product.empty())
        {
            return version.empty() ? product : std::format("{} {}", product, version);
        }
    }
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
    const auto* group = model.findClassByName("OperationGroup");
    const auto* operation = model.findProperty("OperationGroup", "Operation");
    if (group != nullptr && operation != nullptr && model.isA(o.classId, group->id))
    {
        const auto* p = o.find(operation->pid);
        const auto* weak = p != nullptr ? std::get_if<WeakRefProperty>(&p->payload) : nullptr;
        if (const auto definition = weak != nullptr ? document.resolveWeak(weak->tag, weak->key) : std::nullopt; definition && *definition != id)
        {
            return labelOf(document, *definition);
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
    compositions_.reset();
    references_.reset();
    session_->setListener([this](const edit::ChangeSet& changes) -> void {
        references_.reset();
        const bool mobs = mobListChanged(changes);
        emit("doc.changed", { { "changes", changeSetToJson(changes) }, { "info", info() }, { "mobsChanged", mobs } });
    });
}

auto Server::mobListChanged(const edit::ChangeSet& changes) -> bool
{
    if (!session_)
    {
        return true;
    }
    const auto& doc = session_->document();
    const auto& model = doc.model();
    auto classOf = [&](std::string_view name) -> std::optional<Auid> {
        const auto* cls = model.findClassByName(name);
        return cls ? std::optional(cls->id) : std::nullopt;
    };
    const auto mob = classOf("Mob");
    const auto storage = classOf("ContentStorage");
    const auto reference = classOf("SourceReference");
    const auto composition = classOf("CompositionMob");
    const auto* sourceId = model.findProperty("SourceReference", "SourceID");
    auto is = [&](ObjectId id, const std::optional<Auid>& cls) -> bool { return cls && id < doc.objectCount() && model.isA(doc.object(id).classId, *cls); };
    const std::set<ObjectId> created(changes.created.begin(), changes.created.end());
    for (const auto id : changes.objects)
    {
        if (is(id, mob) || is(id, storage))
        {
            compositions_.reset();
            return true;
        }
    }
    if (sourceId != nullptr && std::ranges::any_of(changes.properties, [&](const edit::PropertyChange& p) -> bool { return p.pid == sourceId->pid && !created.contains(p.object) && is(p.object, reference); }))
    {
        return true;
    }
    if (!compositions_)
    {
        compositions_.emplace();
        std::map<Auid, bool> isComposition;
        for (std::size_t i = 0; i < doc.objectCount(); ++i)
        {
            const auto& cls = doc.object(i).classId;
            auto known = isComposition.find(cls);
            if (known == isComposition.end())
            {
                known = isComposition.emplace(cls, is(i, composition)).first;
            }
            if (known->second && doc.isAttached(i))
            {
                if (const auto v = doc.value(i, "Mob", "MobID"); v && v->is<MobId>())
                {
                    compositions_->insert(v->as<MobId>());
                }
            }
        }
    }
    return std::ranges::any_of(changes.objects, [&](ObjectId id) -> bool {
        if (!is(id, reference))
        {
            return false;
        }
        const auto v = doc.value(id, "SourceReference", "SourceID");
        return v && v->is<MobId>() && compositions_->contains(v->as<MobId>());
    });
}

auto Server::references(const Document& document) -> const edit::ReferenceIndex&
{
    if (!references_)
    {
        return references_.emplace(document);
    }
    return *references_;
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
        "object.setReference",
        "labels.family",
        "model.subclasses",
        "edit.undo",
        "edit.redo",
        "edit.history",
        "search.query",
        "essence.extract",
        "essence.replace",
        "timeline.mobs",
        "timeline.get",
        "timeline.resolve",
        "timeline.op",
    };
    if (!kMethods.contains(method))
    {
        return fail(Errc::not_found, std::format("unknown method '{}'", method));
    }
    if (method == "doc.info")
    {
        return info();
    }
    if (method == "timeline.get")
    {
        return timelineText(params).transform([](const std::string& text) -> Json { return Json::parse(text); });
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
        references_.reset();
        session_.reset();
        session_.emplace(std::move(*opened));
        path_ = *path;
        attachListener();
        emit("doc.opened", info());
        return info();
    }
    if (method == "labels.family")
    {
        auto text = param<std::string>(params, "auid");
        if (!text)
        {
            return std::unexpected(text.error());
        }
        auto auid = Auid::parse(*text);
        if (!auid)
        {
            return std::unexpected(auid.error());
        }
        Json out = Json::array();
        for (const auto* label : smpteLabelFamily(*auid))
        {
            out.push_back({ { "auid", label->id.toString() }, { "name", label->name }, { "deprecated", label->deprecated } });
        }
        return out;
    }
    if (method == "doc.close")
    {
        references_.reset();
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
        return objectJson(doc, *id, references(doc));
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
        auto updateReferences = optionalParam<bool>(params, "updateReferences", false);
        if (!value)
        {
            return std::unexpected(value.error());
        }
        if (!updateReferences)
        {
            return std::unexpected(updateReferences.error());
        }
        return run(std::format("Set {}", propertyName(doc, *pid)), [&](edit::Transaction& tx) -> Result<void> {
            return *updateReferences ? edit::setIdentifier(tx, *id, *pid, *value) : edit::setProperty(tx, *id, *pid, *value);
        });
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
        if (!*force)
        {
            if (const auto incoming = edit::incomingImplicitReferences(doc, references(doc), *id); !incoming.empty())
            {
                return invalid(std::format("{} reference(s) point to {} or its descendants by identifier (first: {}.{})", incoming.size(), className(doc, *id), className(doc, incoming.front().object), propertyName(doc, incoming.front().pid)));
            }
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
        const auto& index = references(doc);
        if (index.definition(*id, *pid) != nullptr)
        {
            for (const auto c : index.candidates(*id, *pid))
            {
                if (auto v = index.valueFor(*id, *pid, c))
                {
                    out.push_back({ { "id", c }, { "class", className(doc, c) }, { "label", labelOf(doc, c) }, { "value", toJson(*v) } });
                }
            }
            return out;
        }
        for (const auto c : edit::weakCandidates(doc, *id, *pid))
        {
            out.push_back({ { "id", c }, { "class", className(doc, c) }, { "label", labelOf(doc, c) } });
        }
        return out;
    }
    if (method == "object.setReference")
    {
        auto id = objectParam("id");
        auto pid = param<std::uint16_t>(params, "pid");
        auto target = objectParam("target");
        if (!id || !pid || !target)
        {
            return invalid("object.setReference needs id, pid and target");
        }
        return run(std::format("Set {}", propertyName(doc, *pid)), [&](edit::Transaction& tx) -> Result<void> { return edit::setReference(tx, *id, *pid, *target); });
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
    if (method == "timeline.mobs")
    {
        const timeline::Projector projector(doc);
        Json out = Json::array();
        for (const auto& m : projector.mobs())
        {
            out.push_back({ { "id", m.object }, { "mobId", m.mobId.toString() }, { "name", m.name }, { "kind", std::string(timeline::to_string(m.kind)) }, { "tracks", m.tracks }, { "topLevel", m.topLevel } });
        }
        return out;
    }
    if (method == "timeline.resolve")
    {
        auto clip = objectParam("clip");
        if (!clip)
        {
            return std::unexpected(clip.error());
        }
        const timeline::Projector projector(doc);
        auto chain = projector.resolve(*clip);
        if (!chain)
        {
            return std::unexpected(chain.error());
        }
        Json links = Json::array();
        for (const auto& link : chain->links)
        {
            links.push_back({
                { "mob", link.mob == kNoObject ? Json(nullptr) : Json(link.mob) },
                { "mobId", link.mobId.toString() },
                { "name", link.mobName },
                { "kind", std::string(timeline::to_string(link.mobKind)) },
                { "slotId", link.slotId },
                { "position", link.position },
                { "editRate", { { "num", link.editRate.numerator() }, { "den", link.editRate.denominator() } } },
                { "descriptor", link.descriptor },
            });
        }
        Json essence = nullptr;
        if (chain->essence)
        {
            essence = { { "embedded", chain->essence->embedded }, { "essenceData", chain->essence->essenceData == kNoObject ? Json(nullptr) : Json(chain->essence->essenceData) }, { "locators", chain->essence->locators }, { "descriptor", chain->essence->descriptor } };
        }
        return Json{ { "status", std::string(timeline::to_string(chain->status)) }, { "links", std::move(links) }, { "essence", std::move(essence) } };
    }
    if (method == "timeline.op")
    {
        auto op = param<std::string>(params, "op");
        if (!op)
        {
            return std::unexpected(op.error());
        }
        auto id = [&](const char* key) -> Result<ObjectId> { return objectParam(key); };
        auto integer = [&](const char* key) -> Result<std::int64_t> { return param<std::int64_t>(params, key); };
        Json extra = Json::object();
        edit::Session::Command command;
        std::string description;
        if (*op == "split")
        {
            auto slot = id("slot");
            auto position = integer("position");
            if (!slot || !position)
            {
                return invalid("split needs slot and position");
            }
            description = "Split";
            command = [&, slot = *slot, position = *position](edit::Transaction& tx) -> Result<void> {
                auto right = timeline::ops::split(tx, slot, position);
                if (!right)
                {
                    return std::unexpected(right.error());
                }
                extra["id"] = *right;
                return {};
            };
        }
        else if (*op == "lift" || *op == "rippleDelete")
        {
            auto item = id("item");
            if (!item)
            {
                return std::unexpected(item.error());
            }
            description = *op == "lift" ? "Lift" : "Ripple delete";
            const bool ripple = *op == "rippleDelete";
            command = [item = *item, ripple](edit::Transaction& tx) -> Result<void> { return ripple ? timeline::ops::rippleDelete(tx, item) : timeline::ops::lift(tx, item); };
        }
        else if (*op == "trim")
        {
            auto item = id("item");
            auto edge = param<std::string>(params, "edge");
            auto delta = integer("delta");
            auto ripple = optionalParam<bool>(params, "ripple", false);
            if (!item || !edge || !delta || !ripple || (*edge != "head" && *edge != "tail"))
            {
                return invalid("trim needs item, edge (head or tail) and delta");
            }
            description = *ripple ? "Ripple trim" : "Trim";
            const auto side = *edge == "head" ? timeline::ops::Edge::head : timeline::ops::Edge::tail;
            command = [item = *item, side, delta = *delta, ripple = *ripple](edit::Transaction& tx) -> Result<void> { return timeline::ops::trim(tx, item, side, delta, ripple); };
        }
        else if (*op == "move")
        {
            auto item = id("item");
            auto slot = id("toSlot");
            auto position = integer("position");
            auto ripple = optionalParam<bool>(params, "ripple", false);
            if (!item || !slot || !position || !ripple)
            {
                return invalid("move needs item, toSlot and position");
            }
            description = "Move";
            command = [item = *item, slot = *slot, position = *position, ripple = *ripple](edit::Transaction& tx) -> Result<void> { return timeline::ops::move(tx, item, slot, position, ripple); };
        }
        else if (*op == "insertClip" || *op == "overwriteClip")
        {
            auto slot = id("slot");
            auto position = integer("position");
            auto source = id("sourceMob");
            auto sourceSlot = param<std::uint32_t>(params, "sourceSlot");
            auto sourceIn = integer("sourceIn");
            auto length = integer("length");
            if (!slot || !position || !source || !sourceSlot || !sourceIn || !length)
            {
                return invalid("clip placement needs slot, position, sourceMob, sourceSlot, sourceIn and length");
            }
            const bool insert = *op == "insertClip";
            description = insert ? "Insert clip" : "Overwrite clip";
            command = [&, slot = *slot, position = *position, source = *source, sourceSlot = *sourceSlot, sourceIn = *sourceIn, length = *length, insert](edit::Transaction& tx) -> Result<void> {
                auto clip = timeline::ops::placeClip(tx, slot, position, source, sourceSlot, sourceIn, length, insert);
                if (!clip)
                {
                    return std::unexpected(clip.error());
                }
                extra["id"] = *clip;
                return {};
            };
        }
        else if (*op == "addTrack")
        {
            auto mob = id("mob");
            auto kind = param<std::string>(params, "kind");
            auto name = optionalParam<std::string>(params, "name", {});
            if (!mob || !kind || !name || (*kind != "picture" && *kind != "sound"))
            {
                return invalid("addTrack needs mob and kind (picture or sound)");
            }
            description = "Add track";
            const auto trackKind = *kind == "picture" ? timeline::TrackKind::picture : timeline::TrackKind::sound;
            command = [&, mob = *mob, trackKind, name = *name](edit::Transaction& tx) -> Result<void> {
                auto slot = timeline::ops::addTrack(tx, mob, trackKind, name);
                if (!slot)
                {
                    return std::unexpected(slot.error());
                }
                extra["id"] = *slot;
                return {};
            };
        }
        else if (*op == "removeTrack")
        {
            auto slot = id("slot");
            if (!slot)
            {
                return std::unexpected(slot.error());
            }
            description = "Remove track";
            command = [slot = *slot](edit::Transaction& tx) -> Result<void> { return timeline::ops::removeTrack(tx, slot); };
        }
        else if (*op == "addMarker")
        {
            auto mob = id("mob");
            auto position = integer("position");
            auto comment = optionalParam<std::string>(params, "comment", {});
            if (!mob || !position || !comment)
            {
                return invalid("addMarker needs mob and position");
            }
            description = "Add marker";
            command = [&, mob = *mob, position = *position, comment = *comment](edit::Transaction& tx) -> Result<void> {
                auto marker = timeline::ops::addMarker(tx, mob, position, comment);
                if (!marker)
                {
                    return std::unexpected(marker.error());
                }
                extra["id"] = *marker;
                return {};
            };
        }
        else if (*op == "relink")
        {
            auto find = param<std::string>(params, "find");
            auto replace = optionalParam<std::string>(params, "replace", {});
            if (!find || !replace)
            {
                return invalid("relink needs find and replace");
            }
            description = "Relink media";
            command = [&, find = *find, replace = *replace](edit::Transaction& tx) -> Result<void> {
                auto count = timeline::ops::relink(tx, find, replace);
                if (!count)
                {
                    return std::unexpected(count.error());
                }
                extra["count"] = *count;
                return {};
            };
        }
        else
        {
            return invalid(std::format("unknown timeline operation '{}'", *op));
        }
        auto changes = run(description, command);
        if (!changes)
        {
            return changes;
        }
        extra["changes"] = std::move(*changes);
        return extra;
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

auto Server::timelineText(const Json& params) -> Result<std::string>
{
    auto session = requireSession();
    if (!session)
    {
        return std::unexpected(session.error());
    }
    const auto& doc = (*session)->document();
    auto mob = param<ObjectId>(params, "mob");
    if (mob && *mob >= doc.objectCount())
    {
        return invalid(std::format("object {} does not exist", *mob));
    }
    if (!mob)
    {
        return std::unexpected(mob.error());
    }
    const bool incremental = params.contains("changed") && !params.at("changed").is_null();
    auto changed = incremental ? param<std::vector<ObjectId>>(params, "changed") : Result<std::vector<ObjectId>>{};
    if (!changed)
    {
        return std::unexpected(changed.error());
    }
    const timeline::Projector projector(doc);
    const auto slots = incremental ? projector.affectedSlots(*mob, *changed) : std::nullopt;
    auto t = slots ? projector.project(*mob, *slots) : projector.project(*mob);
    if (!t)
    {
        return std::unexpected(t.error());
    }
    return TimelineWriter().write(*t);
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
    Result<std::string> result = fail(Errc::io, "internal error");
    try
    {
        result = method == "timeline.get" ? timelineText(params) : call(method, params).transform([](const Json& j) -> std::string { return j.dump(); });
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
    return std::format(R"({{"id":{},"jsonrpc":"2.0","result":{}}})", id.dump(), *result);
}

}
