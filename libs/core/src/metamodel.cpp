#include "generated/baseline_tables.hpp"

#include <aaf/core/metamodel.hpp>

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace aaf
{

namespace
{

auto parseId(std::string_view text) -> Auid
{
    if (text.empty())
    {
        return {};
    }
    auto id = Auid::parse(text);
    return id ? *id : Auid{};
}

auto toKind(detail::GenKind kind) -> TypeKind
{
    return static_cast<TypeKind>(kind);
}

auto buildBaseline() -> MetaModel
{
    MetaModel model;
    for (const auto& gt : detail::baselineTypes())
    {
        TypeDef t;
        t.id = parseId(gt.id);
        t.name = gt.name;
        t.kind = toKind(gt.kind);
        t.element = parseId(gt.element);
        t.size = gt.size;
        t.isSigned = gt.isSigned;
        t.count = gt.count;
        for (const auto& f : gt.fields)
        {
            t.fields.push_back({ std::string(f.name), parseId(f.value) });
        }
        for (const auto& e : gt.enumValues)
        {
            t.enumElements.push_back({ std::string(e.name), e.value });
        }
        for (const auto& e : gt.extValues)
        {
            t.extElements.push_back({ std::string(e.name), parseId(e.value) });
        }
        for (const auto& p : gt.path)
        {
            t.targetPath.push_back(parseId(p));
        }
        model.addType(std::move(t), DefinitionSource::baseline);
    }
    for (const auto& gc : detail::baselineClasses())
    {
        ClassDef c;
        c.id = parseId(gc.id);
        c.name = gc.name;
        c.parent = parseId(gc.parent);
        c.concrete = gc.concrete;
        for (const auto& gp : gc.properties)
        {
            PropertyDef p;
            p.id = parseId(gp.id);
            p.name = gp.name;
            p.pid = gp.pid;
            p.type = parseId(gp.type);
            p.owner = c.id;
            p.optional = gp.optional;
            p.uniqueId = gp.unique;
            c.properties.push_back(p.id);
            model.addProperty(std::move(p), DefinitionSource::baseline);
        }
        model.addClass(std::move(c), DefinitionSource::baseline);
    }
    return model;
}

}

auto to_string(TypeKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
        case TypeKind::integer:
            return "integer";
        case TypeKind::enumeration:
            return "enumeration";
        case TypeKind::record:
            return "record";
        case TypeKind::fixed_array:
            return "fixed_array";
        case TypeKind::var_array:
            return "var_array";
        case TypeKind::rename:
            return "rename";
        case TypeKind::string:
            return "string";
        case TypeKind::stream:
            return "stream";
        case TypeKind::opaque:
            return "opaque";
        case TypeKind::ext_enum:
            return "ext_enum";
        case TypeKind::character:
            return "character";
        case TypeKind::generic_character:
            return "generic_character";
        case TypeKind::indirect:
            return "indirect";
        case TypeKind::set:
            return "set";
        case TypeKind::strong_ref:
            return "strong_ref";
        case TypeKind::weak_ref:
            return "weak_ref";
    }
    return "unknown";
}

auto MetaModel::baseline() -> const MetaModel&
{
    static const MetaModel model = buildBaseline();
    return model;
}

auto MetaModel::findClass(const Auid& id) const -> const ClassDef*
{
    const auto it = classes_.find(id);
    return it == classes_.end() ? nullptr : &it->second;
}

auto MetaModel::findType(const Auid& id) const -> const TypeDef*
{
    const auto it = types_.find(id);
    return it == types_.end() ? nullptr : &it->second;
}

auto MetaModel::findProperty(const Auid& id) const -> const PropertyDef*
{
    const auto it = properties_.find(id);
    return it == properties_.end() ? nullptr : &it->second;
}

auto MetaModel::findPropertyByPid(std::uint16_t pid) const -> const PropertyDef*
{
    const auto it = byPid_.find(pid);
    return it == byPid_.end() ? nullptr : findProperty(it->second);
}

auto MetaModel::findClassByName(std::string_view name) const -> const ClassDef*
{
    if (const auto indexed = classesByName_.find(name); indexed != classesByName_.end())
    {
        if (const auto* cls = findClass(indexed->second); cls != nullptr && cls->name == name)
        {
            return cls;
        }
    }
    const auto it = std::ranges::find_if(classes_, [name](const auto& entry) -> auto { return entry.second.name == name; });
    if (it != classes_.end())
    {
        return &it->second;
    }
    if (this != &baseline())
    {
        if (const auto* standard = baseline().findClassByName(name))
        {
            return findClass(standard->id);
        }
    }
    return nullptr;
}

auto MetaModel::findProperty(std::string_view className, std::string_view propertyName) const -> const PropertyDef*
{
    const auto* cls = findClassByName(className);
    if (cls == nullptr)
    {
        return nullptr;
    }
    for (const auto& id : cls->properties)
    {
        const auto* p = findProperty(id);
        if (p != nullptr && p->name == propertyName)
        {
            return p;
        }
    }
    if (this != &baseline())
    {
        if (const auto* standard = baseline().findProperty(className, propertyName); standard != nullptr && std::ranges::contains(cls->properties, standard->id))
        {
            return findProperty(standard->id);
        }
    }
    return nullptr;
}

auto MetaModel::isA(const Auid& id, const Auid& ancestor) const -> bool
{
    Auid current = id;
    for (std::size_t steps = 0; !current.isNull() && steps <= classes_.size(); ++steps)
    {
        if (current == ancestor)
        {
            return true;
        }
        const auto* cls = findClass(current);
        if (cls == nullptr)
        {
            return false;
        }
        current = cls->parent;
    }
    return false;
}

auto MetaModel::allProperties(const Auid& classId) const -> std::vector<const PropertyDef*>
{
    std::vector<const ClassDef*> chain;
    std::unordered_set<Auid> seen;
    for (const auto* cls = findClass(classId); cls != nullptr && seen.insert(cls->id).second; cls = findClass(cls->parent))
    {
        chain.push_back(cls);
    }
    std::vector<const PropertyDef*> result;
    for (auto& it : std::views::reverse(chain))
    {
        for (const auto& id : it->properties)
        {
            if (const auto* p = findProperty(id))
            {
                result.push_back(p);
            }
        }
    }
    return result;
}

auto MetaModel::resolve(const Auid& typeId) const -> const TypeDef*
{
    const TypeDef* t = findType(typeId);
    for (int depth = 0; t != nullptr && t->kind == TypeKind::rename && depth < 16; ++depth)
    {
        t = findType(t->element);
    }
    return t != nullptr && t->kind == TypeKind::rename ? nullptr : t;
}

auto MetaModel::fixedSize(const Auid& typeId) const -> std::optional<std::size_t>
{
    const auto* t = resolve(typeId);
    if (t == nullptr)
    {
        return std::nullopt;
    }
    switch (t->kind)
    {
        case TypeKind::integer:
        case TypeKind::generic_character:
            return t->size;
        case TypeKind::character:
            return 2;
        case TypeKind::enumeration:
            return t->id == ids::kTypeBoolean ? std::optional<std::size_t>(1) : fixedSize(t->element);
        case TypeKind::ext_enum:
            return 16;
        case TypeKind::fixed_array:
        {
            const auto element = fixedSize(t->element);
            return element ? std::optional<std::size_t>(*element * t->count) : std::nullopt;
        }
        case TypeKind::record:
        {
            std::size_t total = 0;
            for (const auto& f : t->fields)
            {
                const auto size = fixedSize(f.type);
                if (!size)
                {
                    return std::nullopt;
                }
                total += *size;
            }
            return total;
        }
        default:
            return std::nullopt;
    }
}

auto MetaModel::sourceOf(const Auid& id) const -> DefinitionSource
{
    const auto it = sources_.find(id);
    return it == sources_.end() ? DefinitionSource::baseline : it->second;
}

void MetaModel::addClass(ClassDef def, DefinitionSource source)
{
    const auto id = def.id;
    sources_[id] = source;
    classesByName_.try_emplace(def.name, id);
    classes_.insert_or_assign(id, std::move(def));
}

void MetaModel::addType(TypeDef def, DefinitionSource source)
{
    const auto id = def.id;
    sources_[id] = source;
    types_.insert_or_assign(id, std::move(def));
}

void MetaModel::addProperty(PropertyDef def, DefinitionSource source)
{
    const auto id = def.id;
    if (const auto it = properties_.find(id); it != properties_.end() && it->second.pid != 0)
    {
        byPid_.erase(it->second.pid);
    }
    if (def.pid != 0)
    {
        byPid_[def.pid] = id;
    }
    sources_[id] = source;
    properties_.insert_or_assign(id, std::move(def));
}

}
