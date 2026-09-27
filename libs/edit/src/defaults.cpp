#include <aaf/edit/defaults.hpp>
#include <aaf/edit/operations.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>

namespace aaf::edit
{

namespace
{

constexpr int kMaxDepth = 8;

auto integerDefault(const TypeDef& t) -> Value
{
    return t.isSigned ? Value(std::int64_t{ 0 }) : Value(std::uint64_t{ 0 });
}

auto now() -> Value
{
    const auto time = std::chrono::system_clock::now();
    const auto days = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day ymd{ days };
    const std::chrono::hh_mm_ss hms{ std::chrono::floor<std::chrono::seconds>(time - days) };
    Value::Record date{ { "year", "month", "day" },
        { Value(std::int64_t{ static_cast<int>(ymd.year()) }), Value(std::uint64_t{ static_cast<unsigned>(ymd.month()) }), Value(std::uint64_t{ static_cast<unsigned>(ymd.day()) }) } };
    Value::Record clock{ { "hour", "minute", "second", "fraction" },
        { Value(static_cast<std::uint64_t>(hms.hours().count())), Value(static_cast<std::uint64_t>(hms.minutes().count())), Value(static_cast<std::uint64_t>(hms.seconds().count())), Value(std::uint64_t{ 0 }) } };
    return Value(Value::Record{ { "date", "time" }, { Value(std::move(date)), Value(std::move(clock)) } });
}

auto typeDefault(const MetaModel& model, const Auid& typeId, bool unique, int depth) -> Result<Value>
{
    if (depth > kMaxDepth)
    {
        return fail(Errc::limit, "type nesting too deep");
    }
    const auto* t = model.resolve(typeId);
    if (t == nullptr)
    {
        return fail(Errc::not_found, std::format("unknown type {}", typeId.toString()));
    }
    switch (t->kind)
    {
        case TypeKind::integer:
            return integerDefault(*t);
        case TypeKind::generic_character:
            return Value(std::uint64_t{ 0 });
        case TypeKind::character:
            return Value(std::string(" "));
        case TypeKind::string:
            return Value(std::string{});
        case TypeKind::enumeration:
            if (t->id == ids::kTypeBoolean)
            {
                return Value(false);
            }
            if (!t->enumElements.empty())
            {
                return Value(Value::Enum{ t->enumElements.front().value, t->enumElements.front().name });
            }
            return Value(Value::Enum{ 0, {} });
        case TypeKind::ext_enum:
            if (!t->extElements.empty())
            {
                return Value(Value::ExtEnum{ t->extElements.front().value, t->extElements.front().name });
            }
            return Value(Auid{});
        case TypeKind::record:
        {
            if (t->id == ids::kTypeAuid)
            {
                return unique ? Value(Auid::generate()) : Value(Auid{});
            }
            if (t->id == ids::kTypeMobId)
            {
                return unique ? Value(MobId::generate()) : Value(MobId{});
            }
            if (t->name == "TimeStamp")
            {
                return now();
            }
            Value::Record record;
            for (const auto& field : t->fields)
            {
                auto v = typeDefault(model, field.type, false, depth + 1);
                if (!v)
                {
                    return v;
                }
                if (field.name == "Denominator" && (v->is<std::int64_t>() || v->is<std::uint64_t>()))
                {
                    v = v->is<std::int64_t>() ? Value(std::int64_t{ 1 }) : Value(std::uint64_t{ 1 });
                }
                record.names.push_back(field.name);
                record.values.push_back(std::move(*v));
            }
            return Value(std::move(record));
        }
        case TypeKind::fixed_array:
        {
            Value::Array items;
            for (std::uint32_t i = 0; i < t->count; ++i)
            {
                auto v = typeDefault(model, t->element, false, depth + 1);
                if (!v)
                {
                    return v;
                }
                items.push_back(std::move(*v));
            }
            return Value(std::move(items));
        }
        case TypeKind::var_array:
        case TypeKind::set:
            return Value(Value::Array{});
        case TypeKind::indirect:
        {
            constexpr auto kInt32 = literals::operator""_auid("01010700-0000-0000-060e-2b3401040101", 36);
            Value::Indirect indirect{ kInt32, {} };
            indirect.value.emplace_back(std::int64_t{ 0 });
            return Value(std::move(indirect));
        }
        case TypeKind::opaque:
            return Value(Value::Opaque{ Auid{}, {} });
        case TypeKind::stream:
        case TypeKind::strong_ref:
        case TypeKind::weak_ref:
        case TypeKind::rename:
            break;
    }
    return fail(Errc::unsupported, std::format("no default for type {}", t->name));
}

auto createImpl(Transaction& tx, const Auid& classId, int depth) -> Result<ObjectId>
{
    if (depth > kMaxDepth)
    {
        return fail(Errc::limit, "object nesting too deep");
    }
    const auto& model = tx.document().model();
    auto id = createObject(tx, classId);
    if (!id)
    {
        return id;
    }
    for (const auto* def : model.allProperties(classId))
    {
        if (def->optional || def->pid == 0 || def->pid == ids::kPidObjClass)
        {
            continue;
        }
        const auto form = expectedStoredForm(model, def->type);
        Result<void> r;
        switch (form.value_or(StoredForm::data))
        {
            case StoredForm::data:
            {
                auto v = defaultValue(model, *def);
                r = v ? setProperty(tx, *id, def->pid, *v) : Result<void>(std::unexpected(v.error()));
                break;
            }
            case StoredForm::strongRef:
            {
                const auto* t = model.resolve(def->type);
                const auto* cls = t == nullptr ? nullptr : concreteClassFor(model, t->element);
                if (cls == nullptr)
                {
                    return fail(Errc::unsupported, std::format("no concrete class for {}", def->name));
                }
                auto child = createImpl(tx, cls->id, depth + 1);
                r = child ? setStrongRef(tx, *id, def->pid, *child) : Result<void>(std::unexpected(child.error()));
                break;
            }
            case StoredForm::strongRefVector:
            case StoredForm::strongRefSet:
                r = ensureCollection(tx, *id, def->pid);
                break;
            case StoredForm::weakRef:
            {
                const auto candidates = weakCandidates(tx.document(), *id, def->pid);
                if (candidates.empty())
                {
                    return fail(Errc::not_found, std::format("{} needs a {} but the file has none", model.findClass(classId)->name, def->name));
                }
                r = setWeakRef(tx, *id, def->pid, candidates.front());
                break;
            }
            case StoredForm::dataStream:
                r = setStreamData(tx, *id, def->pid, std::vector<std::byte>{});
                break;
            default:
                return fail(Errc::unsupported, std::format("cannot create a default for {}", def->name));
        }
        if (!r)
        {
            return std::unexpected(r.error());
        }
    }
    return id;
}

}

auto defaultValue(const MetaModel& model, const PropertyDef& property) -> Result<Value>
{
    return typeDefault(model, property.type, property.uniqueId, 0);
}

auto concreteClassFor(const MetaModel& model, const Auid& classId) -> const ClassDef*
{
    const auto* cls = model.findClass(classId);
    if (cls == nullptr || cls->concrete)
    {
        return cls;
    }
    for (const auto* preferred : { "Sequence", "Filler" })
    {
        const auto* candidate = model.findClassByName(preferred);
        if (candidate != nullptr && model.isA(candidate->id, classId))
        {
            return candidate;
        }
    }
    const ClassDef* best = nullptr;
    for (const auto& [id, c] : model.classes())
    {
        if (c.concrete && model.isA(id, classId) && (best == nullptr || c.name < best->name))
        {
            best = &c;
        }
    }
    return best;
}

auto weakCandidates(const Document& document, ObjectId id, std::uint16_t pid) -> std::vector<ObjectId>
{
    const auto& model = document.model();
    const auto* def = model.findPropertyByPid(pid);
    const auto* t = def == nullptr ? nullptr : model.resolve(def->type);
    if (t != nullptr && (t->kind == TypeKind::var_array || t->kind == TypeKind::set))
    {
        t = model.resolve(t->element);
    }
    if (t == nullptr || t->kind != TypeKind::weak_ref || id >= document.objectCount())
    {
        return {};
    }
    std::vector<ObjectId> out;
    for (std::size_t i = 0; i < document.objectCount(); ++i)
    {
        const auto& o = document.object(i);
        if (o.parent == kNoObject || !model.isA(o.classId, t->element) || !document.isAttached(i))
        {
            continue;
        }
        const auto* holder = document.object(o.parent).find(o.parentPid);
        if (holder != nullptr && std::holds_alternative<StrongRefSetProperty>(holder->payload))
        {
            out.push_back(i);
        }
    }
    return out;
}

auto createWithDefaults(Transaction& tx, const Auid& classId) -> Result<ObjectId>
{
    return createImpl(tx, classId, 0);
}

}
