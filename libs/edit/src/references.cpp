#include <aaf/edit/operations.hpp>
#include <aaf/edit/references.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <set>
#include <tuple>

namespace aaf::edit
{

namespace
{

constexpr std::array kReferences = {
    ImplicitReference{ "SourceReference", "SourceID", "Mob", "Mob", "MobID" },
    ImplicitReference{ "SourceReference", "SourceMobSlotID", "MobSlot", "MobSlot", "SlotID", ReferenceScope::sourceMob },
    ImplicitReference{ "EssenceData", "MobID", "Mob", "Mob", "MobID" },
    ImplicitReference{ "CompositionMob", "Rendering", "Mob", "Mob", "MobID" },
    ImplicitReference{ "FileDescriptor", "LinkedSlotID", "MobSlot", "MobSlot", "SlotID", ReferenceScope::owningMob },
    ImplicitReference{ "Parameter", "Definition", "ParameterDefinition", "DefinitionObject", "Identification" },
    ImplicitReference{ "PluginDefinition", "DefinitionObject", "DefinitionObject", "DefinitionObject", "Identification" },
    ImplicitReference{ "PropertyDefinition", "Type", "TypeDefinition", "MetaDefinition", "Identification" },
    ImplicitReference{ "InterchangeObject", "Generation", "Identification", "Identification", "GenerationAUID" },
};

auto isNullValue(const Value& v) -> bool
{
    if (v.is<MobId>())
    {
        return v.as<MobId>() == MobId{};
    }
    if (v.is<Auid>())
    {
        return v.as<Auid>().isNull();
    }
    return false;
}

auto withStatus(ReferenceStatus status) -> ReferenceResolution
{
    return { .status = status, .target = kNoObject, .builtinName = {} };
}

auto resolvedTo(ObjectId target) -> ReferenceResolution
{
    return { .status = ReferenceStatus::resolved, .target = target, .builtinName = {} };
}

template <typename T>
void appendBytes(std::string& out, const T& value)
{
    const auto raw = std::bit_cast<std::array<char, sizeof(T)>>(value);
    out.append(raw.data(), raw.size());
}

auto classScope(const Auid& cls) -> std::string
{
    std::string out(1, 'c');
    appendBytes(out, cls.bytes);
    return out;
}

auto mobScope(ObjectId mob) -> std::string
{
    std::string out(1, 'm');
    appendBytes(out, mob);
    return out;
}

auto composeKey(std::string scope, std::uint16_t keyPid) -> std::string
{
    appendBytes(scope, keyPid);
    return scope;
}

/// `prefix` followed by a byte form of `value` that is equal for equal identifiers.
auto composeKey(std::string scope, const Value* value) -> std::string
{
    if (value->is<MobId>())
    {
        appendBytes(scope, value->as<MobId>());
    }
    else if (value->is<Auid>())
    {
        appendBytes(scope, value->as<Auid>().bytes);
    }
    else if (value->is<std::uint64_t>())
    {
        appendBytes(scope, value->as<std::uint64_t>());
    }
    else if (value->is<std::int64_t>())
    {
        appendBytes(scope, static_cast<std::uint64_t>(value->as<std::int64_t>()));
    }
    else
    {
        scope += value->toString();
    }
    return scope;
}

auto decodeData(const Document& doc, ObjectId id, std::uint16_t pid) -> std::optional<Value>
{
    const auto& o = doc.object(id);
    const auto* p = o.find(pid);
    if (p == nullptr || !std::holds_alternative<DataProperty>(p->payload))
    {
        return std::nullopt;
    }
    auto v = doc.decode(o, *p);
    return v ? std::optional(std::move(*v)) : std::nullopt;
}

void collectSubtree(const Document& doc, ObjectId id, std::set<ObjectId>& out)
{
    out.insert(id);
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

}

auto implicitReferences() -> std::span<const ImplicitReference>
{
    return kReferences;
}

auto to_string(ReferenceStatus status) -> std::string_view
{
    switch (status)
    {
        case ReferenceStatus::resolved:
            return "resolved";
        case ReferenceStatus::null:
            return "null";
        case ReferenceStatus::external:
            return "external";
        case ReferenceStatus::builtin:
            return "builtin";
    }
    return "external";
}

ReferenceIndex::ReferenceIndex(const Document& document) :
    doc_(document)
{
    const auto& model = doc_.model();
    for (const auto& r : kReferences)
    {
        const auto* owner = model.findClassByName(r.className);
        const auto* target = model.findClassByName(r.targetClass);
        const auto* property = model.findProperty(r.className, r.property);
        const auto* key = model.findProperty(r.keyClass, r.keyProperty);
        if (owner == nullptr || target == nullptr || property == nullptr || key == nullptr || property->pid == 0 || key->pid == 0)
        {
            continue;
        }
        specs_.emplace(property->pid, Spec{ &r, owner->id, target->id, key->pid, composeKey(classScope(target->id), key->pid) });
        for (const auto& cls : { owner->id, target->id })
        {
            if (!std::ranges::contains(classes_, cls))
            {
                classes_.push_back(cls);
            }
        }
    }
    if (const auto* mob = model.findClassByName("Mob"))
    {
        mobClass_ = mob->id;
        if (!std::ranges::contains(classes_, mobClass_))
        {
            classes_.push_back(mobClass_);
        }
    }
    if (const auto* sourceId = model.findProperty("SourceReference", "SourceID"))
    {
        sourceIdPid_ = sourceId->pid;
    }

    std::vector<std::pair<Auid, std::uint16_t>> targets;
    for (const auto& [pid, spec] : specs_)
    {
        if (spec.reference->scope == ReferenceScope::document && !std::ranges::contains(targets, std::pair{ spec.targetClass, spec.keyPid }))
        {
            targets.emplace_back(spec.targetClass, spec.keyPid);
        }
    }
    const auto* slotClass = model.findClassByName("MobSlot");
    const auto* slotId = model.findProperty("MobSlot", "SlotID");
    if (slotClass != nullptr && !std::ranges::contains(classes_, slotClass->id))
    {
        classes_.push_back(slotClass->id);
    }
    std::vector<std::tuple<std::uint64_t, std::uint16_t, std::string>> targetKeys;
    targetKeys.reserve(targets.size());
    for (const auto& [cls, keyPid] : targets)
    {
        targetKeys.emplace_back(bitOf(cls), keyPid, composeKey(classScope(cls), keyPid));
    }
    const auto slotBit = slotClass != nullptr && slotId != nullptr ? bitOf(slotClass->id) : 0;

    std::vector<ObjectId> attached;
    for (std::size_t i = 0; i < doc_.objectCount(); ++i)
    {
        if (doc_.isAttached(i))
        {
            attached.push_back(i);
        }
    }
    for (const auto i : attached)
    {
        const auto mask = membership(i);
        for (const auto& [bit, keyPid, prefix] : targetKeys)
        {
            if ((mask & bit) == 0)
            {
                continue;
            }
            if (const auto v = decodeData(doc_, i, keyPid); v && !isNullValue(*v))
            {
                addKey(prefix, *v, i);
            }
        }
        if ((mask & slotBit) != 0 && doc_.object(i).parent != kNoObject)
        {
            if (const auto v = decodeData(doc_, i, slotId->pid))
            {
                addKey(composeKey(mobScope(doc_.object(i).parent), slotId->pid), *v, i);
            }
        }
    }
    for (const auto i : attached)
    {
        for (const auto& p : doc_.object(i).properties)
        {
            if (const auto* w = std::get_if<WeakRefProperty>(&p.payload))
            {
                if (const auto t = doc_.resolveWeak(w->tag, w->key))
                {
                    incoming_[*t].push_back({ i, p.pid, true });
                }
            }
            else if (const auto* c = std::get_if<WeakRefCollectionProperty>(&p.payload))
            {
                for (const auto& key : c->keys)
                {
                    if (const auto t = doc_.resolveWeak(c->tag, key))
                    {
                        incoming_[*t].push_back({ i, p.pid, true });
                    }
                }
            }
            else if (std::holds_alternative<DataProperty>(p.payload) && specs_.contains(p.pid))
            {
                if (const auto r = resolve(i, p.pid); r && r->status == ReferenceStatus::resolved)
                {
                    incoming_[r->target].push_back({ i, p.pid, false });
                }
            }
        }
    }
}

auto ReferenceIndex::bitOf(const Auid& classId) const -> std::uint64_t
{
    const auto index = static_cast<std::size_t>(std::ranges::find(classes_, classId) - classes_.begin());
    return index < classes_.size() ? std::uint64_t{ 1 } << index : 0;
}

auto ReferenceIndex::isA(ObjectId id, const Auid& classId) const -> bool
{
    const auto bit = bitOf(classId);
    return bit == 0 ? doc_.model().isA(doc_.object(id).classId, classId) : (membership(id) & bit) != 0;
}

auto ReferenceIndex::membership(ObjectId id) const -> std::uint64_t
{
    const auto& cls = doc_.object(id).classId;
    auto it = memberships_.find(cls);
    if (it == memberships_.end())
    {
        std::uint64_t mask = 0;
        for (std::size_t i = 0; i < classes_.size(); ++i)
        {
            mask |= doc_.model().isA(cls, classes_[i]) ? std::uint64_t{ 1 } << i : 0;
        }
        it = memberships_.emplace(cls, mask).first;
    }
    return it->second;
}

void ReferenceIndex::addKey(std::string prefix, const Value& value, ObjectId target)
{
    auto& group = groups_[prefix];
    if (keyed_.try_emplace(composeKey(std::move(prefix), &value), target).second)
    {
        group.push_back(target);
    }
}

auto ReferenceIndex::specFor(ObjectId id, std::uint16_t pid) const -> const Spec*
{
    if (id >= doc_.objectCount())
    {
        return nullptr;
    }
    const auto [first, last] = specs_.equal_range(pid);
    for (auto it = first; it != last; ++it)
    {
        if (isA(id, it->second.ownerClass))
        {
            return &it->second;
        }
    }
    return nullptr;
}

auto ReferenceIndex::definition(ObjectId id, std::uint16_t pid) const -> const ImplicitReference*
{
    const auto* spec = specFor(id, pid);
    return spec == nullptr ? nullptr : spec->reference;
}

auto ReferenceIndex::scopeMob(ObjectId id, const Spec& spec) const -> std::optional<ObjectId>
{
    if (spec.reference->scope == ReferenceScope::owningMob)
    {
        for (ObjectId a = doc_.object(id).parent; a != kNoObject; a = doc_.object(a).parent)
        {
            if (isA(a, mobClass_))
            {
                return a;
            }
        }
        return std::nullopt;
    }
    const auto mob = resolve(id, sourceIdPid_);
    if (!mob || mob->status != ReferenceStatus::resolved)
    {
        return std::nullopt;
    }
    return mob->target;
}

void ReferenceIndex::collectPrefixed(const std::string& prefix, std::vector<ObjectId>& out) const
{
    if (const auto it = groups_.find(prefix); it != groups_.end())
    {
        out.insert(out.end(), it->second.begin(), it->second.end());
    }
}

auto ReferenceIndex::resolve(ObjectId id, std::uint16_t pid) const -> std::optional<ReferenceResolution>
{
    const auto* spec = specFor(id, pid);
    if (spec == nullptr)
    {
        return std::nullopt;
    }
    const auto value = decodeData(doc_, id, pid);
    if (!value)
    {
        return std::nullopt;
    }
    if (spec->reference->scope == ReferenceScope::document)
    {
        if (isNullValue(*value))
        {
            return withStatus(ReferenceStatus::null);
        }
        if (const auto it = keyed_.find(composeKey(spec->prefix, &*value)); it != keyed_.end())
        {
            return resolvedTo(it->second);
        }
        if (value->is<Auid>() && spec->reference->targetClass == "TypeDefinition")
        {
            if (const auto* type = doc_.model().findType(value->as<Auid>()))
            {
                return ReferenceResolution{ ReferenceStatus::builtin, kNoObject, type->name };
            }
        }
        return withStatus(ReferenceStatus::external);
    }
    if (spec->reference->scope == ReferenceScope::sourceMob)
    {
        const auto source = resolve(id, sourceIdPid_);
        if (source && source->status == ReferenceStatus::null)
        {
            return withStatus(ReferenceStatus::null);
        }
    }
    const auto mob = scopeMob(id, *spec);
    if (!mob)
    {
        return withStatus(ReferenceStatus::external);
    }
    if (const auto it = keyed_.find(composeKey(composeKey(mobScope(*mob), spec->keyPid), &*value)); it != keyed_.end())
    {
        return resolvedTo(it->second);
    }
    return withStatus(ReferenceStatus::external);
}

auto ReferenceIndex::candidates(ObjectId id, std::uint16_t pid) const -> std::vector<ObjectId>
{
    const auto* spec = specFor(id, pid);
    if (spec == nullptr)
    {
        return {};
    }
    std::vector<ObjectId> out;
    if (spec->reference->scope == ReferenceScope::document)
    {
        collectPrefixed(spec->prefix, out);
    }
    else if (const auto mob = scopeMob(id, *spec))
    {
        collectPrefixed(composeKey(mobScope(*mob), spec->keyPid), out);
    }
    std::ranges::sort(out);
    return out;
}

auto ReferenceIndex::valueFor(ObjectId id, std::uint16_t pid, ObjectId target) const -> Result<Value>
{
    const auto* spec = specFor(id, pid);
    if (spec == nullptr)
    {
        return fail(Errc::invalid_argument, "the property is not a reference");
    }
    if (!std::ranges::contains(candidates(id, pid), target))
    {
        return fail(Errc::invalid_argument, std::format("object {} cannot be referred to here", target));
    }
    auto v = decodeData(doc_, target, spec->keyPid);
    if (!v)
    {
        return fail(Errc::invalid_argument, std::format("object {} has no {}", target, spec->reference->keyProperty));
    }
    return std::move(*v);
}

auto ReferenceIndex::referrers(ObjectId target) const -> std::span<const Referrer>
{
    const auto it = incoming_.find(target);
    return it == incoming_.end() ? std::span<const Referrer>{} : std::span<const Referrer>(it->second);
}

auto ReferenceIndex::referrersByKey(ObjectId target, std::uint16_t keyPid) const -> std::vector<Referrer>
{
    std::vector<Referrer> out;
    for (const auto& r : referrers(target))
    {
        if (const auto* spec = r.weak ? nullptr : specFor(r.object, r.pid); spec != nullptr && spec->keyPid == keyPid)
        {
            out.push_back(r);
        }
    }
    return out;
}

auto setReference(Transaction& tx, ObjectId id, std::uint16_t pid, ObjectId target) -> Result<void>
{
    auto value = ReferenceIndex(tx.document()).valueFor(id, pid, target);
    if (!value)
    {
        return std::unexpected(value.error());
    }
    return setProperty(tx, id, pid, *value);
}

auto setIdentifier(Transaction& tx, ObjectId id, std::uint16_t pid, const Value& value) -> Result<void>
{
    const auto referrers = ReferenceIndex(tx.document()).referrersByKey(id, pid);
    if (auto r = setProperty(tx, id, pid, value); !r)
    {
        return r;
    }
    for (const auto& referrer : referrers)
    {
        if (auto r = setProperty(tx, referrer.object, referrer.pid, value); !r)
        {
            return r;
        }
    }
    return {};
}

auto incomingImplicitReferences(const Document& document, ObjectId id) -> std::vector<Referrer>
{
    return incomingImplicitReferences(document, ReferenceIndex(document), id);
}

auto incomingImplicitReferences(const Document& document, const ReferenceIndex& index, ObjectId id) -> std::vector<Referrer>
{
    std::set<ObjectId> subtree;
    collectSubtree(document, id, subtree);
    std::vector<Referrer> out;
    for (const auto member : subtree)
    {
        for (const auto& r : index.referrers(member))
        {
            if (!r.weak && !subtree.contains(r.object))
            {
                out.push_back(r);
            }
        }
    }
    return out;
}

}
