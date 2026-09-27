#include <aaf/core/writer.hpp>
#include <aaf/edit/operations.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace aaf::edit
{

namespace
{

constexpr std::uint32_t kNoFreeKey = 0xFFFFFFFF;

auto className(const Document& doc, ObjectId id) -> std::string
{
    const auto* cls = doc.classOf(id);
    return cls != nullptr ? cls->name : doc.object(id).classId.toString();
}

auto checkObject(const Document& doc, ObjectId id) -> Result<void>
{
    if (id == kNoObject || id >= doc.objectCount())
    {
        return fail(Errc::invalid_argument, std::format("object {} does not exist", id));
    }
    return {};
}

auto definitionFor(const Document& doc, ObjectId id, std::uint16_t pid, std::optional<StoredForm> form) -> Result<const PropertyDef*>
{
    if (auto r = checkObject(doc, id); !r)
    {
        return std::unexpected(r.error());
    }
    const auto& model = doc.model();
    const auto* def = model.findPropertyByPid(pid);
    if (def == nullptr)
    {
        return fail(Errc::not_found, std::format("no property with PID {:#06x}", pid));
    }
    const auto props = model.allProperties(doc.object(id).classId);
    if (std::ranges::find(props, def) == props.end())
    {
        return fail(Errc::invalid_argument, std::format("{} has no property {}", className(doc, id), def->name));
    }
    if (form && expectedStoredForm(model, def->type) != form)
    {
        return fail(Errc::invalid_argument, std::format("{}.{} cannot be set this way", className(doc, id), def->name));
    }
    return def;
}

auto elementClass(const MetaModel& model, const PropertyDef& def) -> Auid
{
    const auto* t = model.resolve(def.type);
    if (t != nullptr && (t->kind == TypeKind::var_array || t->kind == TypeKind::set))
    {
        t = model.resolve(t->element);
    }
    return t == nullptr ? Auid{} : t->element;
}

auto checkAttachable(const Document& doc, ObjectId parent, ObjectId child, const PropertyDef& def) -> Result<void>
{
    if (auto r = checkObject(doc, child); !r)
    {
        return r;
    }
    const auto& o = doc.object(child);
    if (child == Document::root() || o.parent != kNoObject)
    {
        return fail(Errc::invalid_argument, std::format("object {} is already attached", child));
    }
    for (ObjectId a = parent; a != kNoObject; a = doc.object(a).parent)
    {
        if (a == child)
        {
            return fail(Errc::invalid_argument, "attaching the object would create a cycle");
        }
    }
    const auto required = elementClass(doc.model(), def);
    if (!required.isNull() && !doc.model().isA(o.classId, required))
    {
        return fail(Errc::invalid_argument, std::format("{} cannot hold a {}", def.name, className(doc, child)));
    }
    return {};
}

void attach(Transaction& tx, ObjectId child, ObjectId parent, std::uint16_t pid)
{
    auto& o = tx.touch(child);
    o.parent = parent;
    o.parentPid = pid;
}

void detach(Transaction& tx, ObjectId child)
{
    auto& o = tx.touch(child);
    o.parent = kNoObject;
    o.parentPid = 0;
}

auto newName(const PropertyDef& def) -> std::u16string
{
    return generatedStorageName(def.name, def.pid);
}

void collectSubtree(const Document& doc, ObjectId id, std::vector<ObjectId>& out)
{
    out.push_back(id);
    for (const auto& p : doc.object(id).properties)
    {
        if (const auto* s = std::get_if<StrongRefProperty>(&p.payload))
        {
            collectSubtree(doc, s->object, out);
        }
        else if (const auto* v = std::get_if<StrongRefVectorProperty>(&p.payload))
        {
            for (const auto c : v->objects)
            {
                collectSubtree(doc, c, out);
            }
        }
        else if (const auto* set = std::get_if<StrongRefSetProperty>(&p.payload))
        {
            for (const auto c : set->objects)
            {
                collectSubtree(doc, c, out);
            }
        }
    }
}

/// Tags whose target set is property `pid` of object `owner`.
auto tagsFor(const Document& doc, ObjectId owner, std::uint16_t pid) -> std::vector<std::uint16_t>
{
    std::vector<std::uint16_t> tags;
    for (std::size_t tag = 0; tag < doc.referencedProperties().size(); ++tag)
    {
        if (doc.tagTarget(static_cast<std::uint16_t>(tag)) == std::pair{ owner, pid })
        {
            tags.push_back(static_cast<std::uint16_t>(tag));
        }
    }
    return tags;
}

/// Number of weak references in the document that point into the subtree rooted at `id`.
auto incomingReferences(const Document& doc, ObjectId id) -> std::size_t
{
    std::vector<ObjectId> subtree;
    collectSubtree(doc, id, subtree);
    std::set<std::pair<std::uint16_t, std::vector<std::byte>>> targets;
    for (const auto member : subtree)
    {
        const auto& o = doc.object(member);
        if (o.parent == kNoObject)
        {
            continue;
        }
        const auto* p = doc.object(o.parent).find(o.parentPid);
        const auto* set = p == nullptr ? nullptr : std::get_if<StrongRefSetProperty>(&p->payload);
        if (set == nullptr)
        {
            continue;
        }
        const auto it = std::ranges::find(set->objects, member);
        const auto& key = set->entries[static_cast<std::size_t>(it - set->objects.begin())].key;
        for (const auto tag : tagsFor(doc, o.parent, o.parentPid))
        {
            targets.emplace(tag, key);
        }
    }
    if (targets.empty())
    {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        for (const auto& p : doc.object(i).properties)
        {
            if (const auto* w = std::get_if<WeakRefProperty>(&p.payload))
            {
                count += targets.contains({ w->tag, w->key }) ? 1U : 0U;
            }
            else if (const auto* c = std::get_if<WeakRefCollectionProperty>(&p.payload))
            {
                for (const auto& key : c->keys)
                {
                    count += targets.contains({ c->tag, key }) ? 1U : 0U;
                }
            }
        }
    }
    return count;
}

auto checkUnreferenced(const Document& doc, ObjectId id) -> Result<void>
{
    if (const auto n = incomingReferences(doc, id); n > 0)
    {
        return fail(Errc::invalid_argument, std::format("{} weak reference(s) point to {} or its descendants", n, className(doc, id)));
    }
    return {};
}

void renameWeakKeys(Transaction& tx, const std::vector<std::uint16_t>& tags, const std::vector<std::byte>& oldKey, const std::vector<std::byte>& newKey)
{
    const auto& doc = tx.document();
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        const auto& o = doc.object(i);
        const bool affected = std::ranges::any_of(o.properties, [&](const Property& p) -> bool {
            if (const auto* w = std::get_if<WeakRefProperty>(&p.payload))
            {
                return std::ranges::contains(tags, w->tag) && w->key == oldKey;
            }
            if (const auto* c = std::get_if<WeakRefCollectionProperty>(&p.payload))
            {
                return std::ranges::contains(tags, c->tag) && std::ranges::contains(c->keys, oldKey);
            }
            return false;
        });
        if (!affected)
        {
            continue;
        }
        for (auto& p : tx.touch(i).properties)
        {
            if (auto* w = std::get_if<WeakRefProperty>(&p.payload); w != nullptr && std::ranges::contains(tags, w->tag) && w->key == oldKey)
            {
                w->key = newKey;
            }
            else if (auto* c = std::get_if<WeakRefCollectionProperty>(&p.payload); c != nullptr && std::ranges::contains(tags, c->tag))
            {
                std::ranges::replace(c->keys, oldKey, newKey);
            }
        }
    }
}

auto updateSetKey(Transaction& tx, ObjectId id, std::uint16_t pid, const std::vector<std::byte>& newKey) -> Result<void>
{
    const auto& doc = tx.document();
    const auto& o = doc.object(id);
    if (o.parent == kNoObject)
    {
        return {};
    }
    const auto* p = doc.object(o.parent).find(o.parentPid);
    const auto* set = p == nullptr ? nullptr : std::get_if<StrongRefSetProperty>(&p->payload);
    if (set == nullptr || set->keyPid != pid)
    {
        return {};
    }
    if (newKey.size() != set->keySize)
    {
        return fail(Errc::invalid_argument, "unique identifier has the wrong size");
    }
    const auto index = static_cast<std::size_t>(std::ranges::find(set->objects, id) - set->objects.begin());
    const auto oldKey = set->entries[index].key;
    if (oldKey == newKey)
    {
        return {};
    }
    if (std::ranges::any_of(set->entries, [&](const SetEntry& e) -> bool { return e.key == newKey; }))
    {
        return fail(Errc::invalid_argument, std::format("another element already has identifier {}", formatKey(newKey)));
    }
    const auto tags = tagsFor(doc, o.parent, o.parentPid);
    std::get<StrongRefSetProperty>(tx.touch(o.parent).find(o.parentPid)->payload).entries[index].key = newKey;
    renameWeakKeys(tx, tags, oldKey, newKey);
    return {};
}

auto uniqueKeyPid(const MetaModel& model, const Auid& classId) -> std::optional<std::uint16_t>
{
    for (const auto* p : model.allProperties(classId))
    {
        if (p->uniqueId && p->pid != 0)
        {
            return p->pid;
        }
    }
    return std::nullopt;
}

auto dataBytes(const Object& o, std::uint16_t pid) -> const std::vector<std::byte>*
{
    const auto* p = o.find(pid);
    const auto* d = p == nullptr ? nullptr : std::get_if<DataProperty>(&p->payload);
    return d == nullptr ? nullptr : &d->bytes;
}

void setPayload(Object& o, std::uint16_t pid, StoredForm form, PropertyPayload payload)
{
    if (auto* p = o.find(pid))
    {
        p->storedForm = static_cast<std::uint16_t>(form);
        p->payload = std::move(payload);
        return;
    }
    o.properties.push_back(Property{ pid, static_cast<std::uint16_t>(form), std::move(payload) });
}

}

auto pidOf(const Document& document, ObjectId id, std::string_view propertyName) -> Result<std::uint16_t>
{
    if (auto r = checkObject(document, id); !r)
    {
        return std::unexpected(r.error());
    }
    for (const auto* p : document.model().allProperties(document.object(id).classId))
    {
        if (p->name == propertyName && p->pid != 0)
        {
            return p->pid;
        }
    }
    return fail(Errc::not_found, std::format("{} has no property {}", className(document, id), propertyName));
}

auto setProperty(Transaction& tx, ObjectId id, std::uint16_t pid, const Value& value) -> Result<void>
{
    auto def = definitionFor(tx.document(), id, pid, StoredForm::data);
    if (!def)
    {
        return std::unexpected(def.error());
    }
    auto bytes = encodeValue(tx.document().model(), (*def)->type, value, tx.document().object(id).bigEndian());
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    if (bytes->size() > 0xFFFF)
    {
        return fail(Errc::limit, std::format("{} value exceeds 65535 bytes", (*def)->name));
    }
    if (auto r = updateSetKey(tx, id, pid, *bytes); !r)
    {
        return r;
    }
    setPayload(tx.touch(id), pid, StoredForm::data, DataProperty{ std::move(*bytes) });
    return {};
}

auto removeProperty(Transaction& tx, ObjectId id, std::uint16_t pid) -> Result<void>
{
    const auto& doc = tx.document();
    if (auto r = checkObject(doc, id); !r)
    {
        return r;
    }
    const auto* p = doc.object(id).find(pid);
    if (p == nullptr)
    {
        return fail(Errc::not_found, std::format("{} has no property {:#06x}", className(doc, id), pid));
    }
    const auto* def = doc.model().findPropertyByPid(pid);
    if (def != nullptr && !def->optional)
    {
        return fail(Errc::invalid_argument, std::format("{}.{} is required", className(doc, id), def->name));
    }
    std::vector<ObjectId> children;
    if (const auto* s = std::get_if<StrongRefProperty>(&p->payload))
    {
        children.push_back(s->object);
    }
    else if (const auto* v = std::get_if<StrongRefVectorProperty>(&p->payload))
    {
        children = v->objects;
    }
    else if (const auto* set = std::get_if<StrongRefSetProperty>(&p->payload))
    {
        children = set->objects;
    }
    for (const auto child : children)
    {
        if (auto r = checkUnreferenced(doc, child); !r)
        {
            return r;
        }
    }
    for (const auto child : children)
    {
        detach(tx, child);
    }
    std::erase_if(tx.touch(id).properties, [pid](const Property& x) -> bool { return x.pid == pid; });
    return {};
}

auto createObject(Transaction& tx, const Auid& classId) -> Result<ObjectId>
{
    const auto* cls = tx.document().model().findClass(classId);
    if (cls == nullptr)
    {
        return fail(Errc::not_found, std::format("unknown class {}", classId.toString()));
    }
    if (!cls->concrete)
    {
        return fail(Errc::invalid_argument, std::format("class {} is abstract", cls->name));
    }
    return tx.create(classId);
}

auto setStrongRef(Transaction& tx, ObjectId parent, std::uint16_t pid, ObjectId child) -> Result<void>
{
    const auto& doc = tx.document();
    auto def = definitionFor(doc, parent, pid, StoredForm::strongRef);
    if (!def)
    {
        return std::unexpected(def.error());
    }
    if (auto r = checkAttachable(doc, parent, child, **def); !r)
    {
        return r;
    }
    std::u16string name = newName(**def);
    if (const auto* p = doc.object(parent).find(pid))
    {
        const auto& old = std::get<StrongRefProperty>(p->payload);
        if (auto r = checkUnreferenced(doc, old.object); !r)
        {
            return r;
        }
        name = old.name;
        detach(tx, old.object);
    }
    setPayload(tx.touch(parent), pid, StoredForm::strongRef, StrongRefProperty{ name, child });
    attach(tx, child, parent, pid);
    return {};
}

auto ensureCollection(Transaction& tx, ObjectId parent, std::uint16_t pid) -> Result<void>
{
    const auto& doc = tx.document();
    auto def = definitionFor(doc, parent, pid, std::nullopt);
    if (!def)
    {
        return std::unexpected(def.error());
    }
    if (doc.object(parent).find(pid) != nullptr)
    {
        return {};
    }
    const auto form = expectedStoredForm(doc.model(), (*def)->type);
    if (form == StoredForm::strongRefVector)
    {
        setPayload(tx.touch(parent), pid, StoredForm::strongRefVector, StrongRefVectorProperty{ newName(**def), {}, {}, 0, kNoFreeKey });
        return {};
    }
    if (form == StoredForm::strongRefSet)
    {
        const auto element = elementClass(doc.model(), **def);
        const auto keyPid = uniqueKeyPid(doc.model(), element);
        const auto* keyDef = keyPid ? doc.model().findPropertyByPid(*keyPid) : nullptr;
        const auto keySize = keyDef != nullptr ? doc.model().fixedSize(keyDef->type) : std::nullopt;
        if (!keyPid || !keySize)
        {
            return fail(Errc::invalid_argument, std::format("{} elements have no fixed-size unique identifier", (*def)->name));
        }
        setPayload(tx.touch(parent), pid, StoredForm::strongRefSet, StrongRefSetProperty{ newName(**def), {}, {}, 0, kNoFreeKey, *keyPid, static_cast<std::uint8_t>(*keySize) });
        return {};
    }
    return fail(Errc::invalid_argument, std::format("{} is not a strong reference collection", (*def)->name));
}

auto insertIntoCollection(Transaction& tx, ObjectId parent, std::uint16_t pid, std::size_t index, ObjectId child) -> Result<void>
{
    const auto& doc = tx.document();
    auto def = definitionFor(doc, parent, pid, std::nullopt);
    if (!def)
    {
        return std::unexpected(def.error());
    }
    const auto form = expectedStoredForm(doc.model(), (*def)->type);
    if (form != StoredForm::strongRefVector && form != StoredForm::strongRefSet)
    {
        return fail(Errc::invalid_argument, std::format("{} is not a strong reference collection", (*def)->name));
    }
    if (auto r = checkAttachable(doc, parent, child, **def); !r)
    {
        return r;
    }
    const auto* existing = doc.object(parent).find(pid);
    const bool asSet = existing != nullptr ? std::holds_alternative<StrongRefSetProperty>(existing->payload) : form == StoredForm::strongRefSet;
    if (!asSet)
    {
        StrongRefVectorProperty v = existing != nullptr ? std::get<StrongRefVectorProperty>(existing->payload) : StrongRefVectorProperty{ newName(**def), {}, {}, 0, kNoFreeKey };
        if (index > v.objects.size())
        {
            return fail(Errc::invalid_argument, std::format("index {} is out of range", index));
        }
        const auto key = v.firstFreeKey++;
        v.objects.insert(v.objects.begin() + static_cast<std::ptrdiff_t>(index), child);
        v.localKeys.insert(v.localKeys.begin() + static_cast<std::ptrdiff_t>(index), key);
        setPayload(tx.touch(parent), pid, existing != nullptr ? static_cast<StoredForm>(existing->storedForm) : StoredForm::strongRefVector, std::move(v));
        attach(tx, child, parent, pid);
        return {};
    }

    StrongRefSetProperty s;
    if (existing != nullptr)
    {
        s = std::get<StrongRefSetProperty>(existing->payload);
    }
    else
    {
        const auto keyPid = uniqueKeyPid(doc.model(), doc.object(child).classId);
        if (!keyPid)
        {
            return fail(Errc::invalid_argument, std::format("{} has no unique identifier", className(doc, child)));
        }
        s = StrongRefSetProperty{ newName(**def), {}, {}, 0, kNoFreeKey, *keyPid, 0 };
    }
    const auto* key = dataBytes(doc.object(child), s.keyPid);
    if (key == nullptr || key->empty())
    {
        return fail(Errc::invalid_argument, std::format("{} must have its unique identifier set before insertion", className(doc, child)));
    }
    if (s.objects.empty() && existing == nullptr)
    {
        s.keySize = static_cast<std::uint8_t>(key->size());
    }
    if (key->size() != s.keySize)
    {
        return fail(Errc::invalid_argument, "unique identifier has the wrong size");
    }
    if (std::ranges::any_of(s.entries, [key](const SetEntry& e) -> bool { return e.key == *key; }))
    {
        return fail(Errc::invalid_argument, std::format("the set already contains {}", formatKey(*key)));
    }
    s.entries.push_back(SetEntry{ s.firstFreeKey++, 1, *key });
    s.objects.push_back(child);
    setPayload(tx.touch(parent), pid, StoredForm::strongRefSet, std::move(s));
    attach(tx, child, parent, pid);
    return {};
}

auto removeFromCollection(Transaction& tx, ObjectId parent, std::uint16_t pid, std::size_t index) -> Result<ObjectId>
{
    const auto& doc = tx.document();
    if (auto r = checkObject(doc, parent); !r)
    {
        return std::unexpected(r.error());
    }
    const auto* p = doc.object(parent).find(pid);
    if (p == nullptr)
    {
        return fail(Errc::not_found, std::format("{} has no property {:#06x}", className(doc, parent), pid));
    }
    ObjectId child = kNoObject;
    if (const auto* v = std::get_if<StrongRefVectorProperty>(&p->payload); v != nullptr && index < v->objects.size())
    {
        child = v->objects[index];
        auto& target = std::get<StrongRefVectorProperty>(tx.touch(parent).find(pid)->payload);
        target.objects.erase(target.objects.begin() + static_cast<std::ptrdiff_t>(index));
        target.localKeys.erase(target.localKeys.begin() + static_cast<std::ptrdiff_t>(index));
    }
    else if (const auto* s = std::get_if<StrongRefSetProperty>(&p->payload); s != nullptr && index < s->objects.size())
    {
        child = s->objects[index];
        auto& target = std::get<StrongRefSetProperty>(tx.touch(parent).find(pid)->payload);
        target.objects.erase(target.objects.begin() + static_cast<std::ptrdiff_t>(index));
        target.entries.erase(target.entries.begin() + static_cast<std::ptrdiff_t>(index));
    }
    else
    {
        return fail(Errc::invalid_argument, std::format("no element {} in a strong reference collection", index));
    }
    detach(tx, child);
    return child;
}

auto moveInCollection(Transaction& tx, ObjectId parent, std::uint16_t pid, std::size_t from, std::size_t to) -> Result<void>
{
    const auto& doc = tx.document();
    if (auto r = checkObject(doc, parent); !r)
    {
        return r;
    }
    const auto* p = doc.object(parent).find(pid);
    const auto* v = p == nullptr ? nullptr : std::get_if<StrongRefVectorProperty>(&p->payload);
    if (v == nullptr || from >= v->objects.size() || to >= v->objects.size())
    {
        return fail(Errc::invalid_argument, "not a strong reference vector, or index out of range");
    }
    if (from == to)
    {
        return {};
    }
    auto& target = std::get<StrongRefVectorProperty>(tx.touch(parent).find(pid)->payload);
    auto move = [from, to](auto& items) -> auto {
        auto value = items[from];
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), value);
    };
    move(target.objects);
    move(target.localKeys);
    return {};
}

auto deleteObject(Transaction& tx, ObjectId id, bool force) -> Result<void>
{
    const auto& doc = tx.document();
    if (auto r = checkObject(doc, id); !r)
    {
        return r;
    }
    const auto& o = doc.object(id);
    if (id == Document::root() || o.parent == kNoObject)
    {
        return fail(Errc::invalid_argument, std::format("object {} is not attached", id));
    }
    if (force)
    {
        tx.allowDanglingReferences();
    }
    else if (auto r = checkUnreferenced(doc, id); !r)
    {
        return r;
    }
    const auto parent = o.parent;
    const auto* p = doc.object(parent).find(o.parentPid);
    if (std::holds_alternative<StrongRefProperty>(p->payload))
    {
        detach(tx, id);
        std::erase_if(tx.touch(parent).properties, [pid = p->pid](const Property& x) -> bool { return x.pid == pid; });
        return {};
    }
    const std::vector<ObjectId>& siblings = std::holds_alternative<StrongRefVectorProperty>(p->payload) ? std::get<StrongRefVectorProperty>(p->payload).objects
                                                                                                        : std::get<StrongRefSetProperty>(p->payload).objects;
    const auto index = static_cast<std::size_t>(std::ranges::find(siblings, id) - siblings.begin());
    auto removed = removeFromCollection(tx, parent, p->pid, index);
    if (!removed)
    {
        return std::unexpected(removed.error());
    }
    return {};
}

auto setWeakRef(Transaction& tx, ObjectId id, std::uint16_t pid, ObjectId target) -> Result<void>
{
    const auto& doc = tx.document();
    auto def = definitionFor(doc, id, pid, StoredForm::weakRef);
    if (!def)
    {
        return std::unexpected(def.error());
    }
    if (auto r = checkObject(doc, target); !r || !doc.isAttached(target))
    {
        return fail(Errc::invalid_argument, std::format("object {} is not attached", target));
    }
    const auto& t = doc.object(target);
    const auto required = elementClass(doc.model(), **def);
    if (!required.isNull() && !doc.model().isA(t.classId, required))
    {
        return fail(Errc::invalid_argument, std::format("{} cannot refer to a {}", (*def)->name, className(doc, target)));
    }
    const auto* holder = doc.object(t.parent).find(t.parentPid);
    const auto* set = holder == nullptr ? nullptr : std::get_if<StrongRefSetProperty>(&holder->payload);
    if (set == nullptr)
    {
        return fail(Errc::invalid_argument, "weak references can only refer to elements of a strong reference set");
    }
    std::vector<std::uint16_t> path;
    for (ObjectId a = target; a != Document::root(); a = doc.object(a).parent)
    {
        path.insert(path.begin(), doc.object(a).parentPid);
    }
    auto tags = tagsFor(doc, t.parent, t.parentPid);
    std::uint16_t tag = 0;
    if (!tags.empty())
    {
        tag = tags.front();
    }
    else
    {
        auto& referenced = tx.referencedProperties();
        if (referenced.size() >= 0xFFFF)
        {
            return fail(Errc::limit, "too many referenced properties");
        }
        tag = static_cast<std::uint16_t>(referenced.size());
        referenced.push_back(std::move(path));
    }
    const auto index = static_cast<std::size_t>(std::ranges::find(set->objects, target) - set->objects.begin());
    setPayload(tx.touch(id), pid, StoredForm::weakRef, WeakRefProperty{ tag, set->keyPid, set->entries[index].key });
    return {};
}

auto setStreamData(Transaction& tx, ObjectId id, std::uint16_t pid, std::vector<std::byte> data) -> Result<void>
{
    auto def = definitionFor(tx.document(), id, pid, StoredForm::dataStream);
    if (!def)
    {
        return std::unexpected(def.error());
    }
    StreamProperty stream;
    if (const auto* p = tx.document().object(id).find(pid))
    {
        stream = std::get<StreamProperty>(p->payload);
    }
    else
    {
        stream.name = newName(**def);
    }
    stream.size = data.size();
    stream.data = std::make_shared<const std::vector<std::byte>>(std::move(data));
    setPayload(tx.touch(id), pid, StoredForm::dataStream, std::move(stream));
    return {};
}

}
