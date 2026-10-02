#include <aaf/core/document.hpp>

#include <format>
#include <unordered_map>

namespace aaf
{

namespace
{

}

auto expectedStoredForm(const MetaModel& model, const Auid& typeId) -> std::optional<StoredForm>
{
    const auto* t = model.resolve(typeId);
    if (t == nullptr)
    {
        return std::nullopt;
    }
    switch (t->kind)
    {
        case TypeKind::strong_ref:
            return StoredForm::strongRef;
        case TypeKind::weak_ref:
            return StoredForm::weakRef;
        case TypeKind::stream:
            return StoredForm::dataStream;
        case TypeKind::var_array:
        case TypeKind::set:
        {
            const auto* element = model.resolve(t->element);
            const bool isSet = t->kind == TypeKind::set;
            if (element != nullptr && element->kind == TypeKind::strong_ref)
            {
                return isSet ? StoredForm::strongRefSet : StoredForm::strongRefVector;
            }
            if (element != nullptr && element->kind == TypeKind::weak_ref)
            {
                return isSet ? StoredForm::weakRefSet : StoredForm::weakRefVector;
            }
            return StoredForm::data;
        }
        default:
            return StoredForm::data;
    }
}

namespace
{

auto collectionQuirk(StoredForm expected, std::uint16_t actual) -> bool
{
    const auto a = static_cast<StoredForm>(actual);
    auto pair = [&](StoredForm x, StoredForm y) -> bool { return (expected == x && a == y) || (expected == y && a == x); };
    return pair(StoredForm::strongRefVector, StoredForm::strongRefSet) || pair(StoredForm::weakRefVector, StoredForm::weakRefSet);
}

}

auto isKnownDefinitionKey(const MetaModel& model, std::span<const std::byte> key, bool bigEndian) -> bool
{
    if (key.size() != 16)
    {
        return false;
    }
    const auto id = Auid::fromStored(key.first<16>(), bigEndian);
    return model.findClass(id) != nullptr || model.findType(id) != nullptr || model.findProperty(id) != nullptr;
}

namespace
{

class Validator
{
public:
    explicit Validator(const Document& doc) :
        doc_(doc),
        model_(doc.model())
    {
    }

    auto run() -> std::vector<Diagnostic>
    {
        out_ = doc_.loadDiagnostics();
        for (std::size_t i = 1; i < doc_.objectCount(); ++i)
        {
            if (doc_.isAttached(i))
            {
                checkObject(doc_.object(i));
            }
        }
        return std::move(out_);
    }

    auto runOne(ObjectId id) -> std::vector<Diagnostic>
    {
        checkObject(doc_.object(id));
        return std::move(out_);
    }

private:
    void report(Diagnostic::Severity severity, const Object& o, std::uint16_t pid, std::string message)
    {
        out_.push_back({ severity, o.id, pid, std::move(message) });
    }

    void checkObject(const Object& o)
    {
        const auto* cls = model_.findClass(o.classId);
        if (cls == nullptr)
        {
            report(Diagnostic::Severity::warning, o, 0, std::format("unknown class {}", o.classId.toString()));
            return;
        }
        std::unordered_map<std::uint16_t, const PropertyDef*> allowed;
        for (const auto* def : model_.allProperties(o.classId))
        {
            if (def->pid != 0)
            {
                allowed.emplace(def->pid, def);
            }
        }
        for (const auto& p : o.properties)
        {
            checkProperty(o, *cls, allowed, p);
        }
        for (const auto& [pid, def] : allowed)
        {
            if (!def->optional && pid != ids::kPidObjClass && o.find(pid) == nullptr)
            {
                report(Diagnostic::Severity::error, o, pid, std::format("{} is missing required property {}", cls->name, def->name));
            }
        }
        checkChannelCombiner(o);
    }

    /// The inputs of an audio channel combiner are the channels of one clip, so each must be as long as the group.
    void checkChannelCombiner(const Object& o)
    {
        const auto* group = model_.findClassByName("OperationGroup");
        const auto* operation = model_.findProperty("OperationGroup", "Operation");
        const auto* inputs = model_.findProperty("OperationGroup", "InputSegments");
        const auto* length = model_.findProperty("Component", "Length");
        if (group == nullptr || operation == nullptr || inputs == nullptr || length == nullptr || !model_.isA(o.classId, group->id))
        {
            return;
        }
        const auto* op = o.find(operation->pid);
        const auto* weak = op == nullptr ? nullptr : std::get_if<WeakRefProperty>(&op->payload);
        if (weak == nullptr || weak->key.size() != 16)
        {
            return;
        }
        const auto key = Auid::fromStored(std::span<const std::byte, 16>(weak->key.data(), 16), o.bigEndian());
        const auto definition = doc_.resolveWeak(weak->tag, weak->key);
        const auto name = definition ? doc_.value(*definition, "DefinitionObject", "Name") : std::nullopt;
        const bool combiner = key == kChannelCombiner || (name && name->is<std::string>() && name->as<std::string>() == "Audio Channel Combiner");
        const auto* list = o.find(inputs->pid);
        const auto* vector = list == nullptr ? nullptr : std::get_if<StrongRefVectorProperty>(&list->payload);
        const auto groupLength = doc_.value(o.id, "Component", "Length");
        if (!combiner || vector == nullptr || !groupLength)
        {
            return;
        }
        for (std::size_t i = 0; i < vector->objects.size(); ++i)
        {
            const auto inputLength = doc_.value(vector->objects[i], "Component", "Length");
            if (inputLength && *inputLength != *groupLength)
            {
                report(Diagnostic::Severity::error, o, inputs->pid, std::format("audio channel {} has length {}, but its channel combiner has length {}", i + 1, inputLength->toString(), groupLength->toString()));
            }
        }
    }

    static constexpr Auid kChannelCombiner = [] consteval -> Auid {
        using namespace aaf::literals;
        return "6b46dd7a-132d-4856-ab21-8b751d8462ec"_auid;
    }();

    void checkProperty(const Object& o, const ClassDef& cls, const std::unordered_map<std::uint16_t, const PropertyDef*>& allowed, const Property& p)
    {
        const auto* def = model_.findPropertyByPid(p.pid);
        if (def == nullptr)
        {
            report(Diagnostic::Severity::warning, o, p.pid, std::format("{} has undefined property {:#06x}", cls.name, p.pid));
            return;
        }
        if (!allowed.contains(p.pid))
        {
            report(Diagnostic::Severity::warning, o, p.pid, std::format("{} has property {} which its class does not define", cls.name, def->name));
        }
        if (const auto expected = expectedStoredForm(model_, def->type); expected && static_cast<std::uint16_t>(*expected) != p.storedForm)
        {
            const auto severity = collectionQuirk(*expected, p.storedForm) ? Diagnostic::Severity::info : Diagnostic::Severity::error;
            report(severity, o, p.pid, std::format("{}.{} has stored form {:#04x}, expected {:#04x}", cls.name, def->name, p.storedForm, static_cast<std::uint16_t>(*expected)));
            if (severity == Diagnostic::Severity::error)
            {
                return;
            }
        }
        if (std::holds_alternative<DataProperty>(p.payload))
        {
            if (auto v = doc_.decode(o, p); !v)
            {
                report(Diagnostic::Severity::error, o, p.pid, std::format("{}.{}: {}", cls.name, def->name, v.error().message));
            }
        }
        else if (const auto* w = std::get_if<WeakRefProperty>(&p.payload))
        {
            checkWeak(o, *def, w->tag, w->key);
        }
        else if (const auto* c = std::get_if<WeakRefCollectionProperty>(&p.payload))
        {
            for (const auto& key : c->keys)
            {
                checkWeak(o, *def, c->tag, key);
            }
        }
        else if (std::holds_alternative<UnknownProperty>(p.payload))
        {
            report(Diagnostic::Severity::warning, o, p.pid, std::format("{}.{} has unknown stored form {:#06x}", cls.name, def->name, p.storedForm));
        }
    }

    void checkWeak(const Object& o, const PropertyDef& def, std::uint16_t tag, std::span<const std::byte> key)
    {
        if (doc_.resolveWeak(tag, key) || isKnownDefinitionKey(model_, key, o.bigEndian()))
        {
            return;
        }
        report(Diagnostic::Severity::error, o, def.pid, std::format("{} refers to missing object {}", def.name, formatKey(key, o.bigEndian())));
    }

    const Document& doc_;
    const MetaModel& model_;
    std::vector<Diagnostic> out_;
};

}

auto validate(const Document& document) -> std::vector<Diagnostic>
{
    return Validator(document).run();
}

auto validateObject(const Document& document, ObjectId id) -> std::vector<Diagnostic>
{
    if (id == Document::root())
    {
        return {};
    }
    return Validator(document).runOne(id);
}

}
