#pragma once

#include <aaf/core/document.hpp>
#include <aaf/timeline/rational.hpp>

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aaf::timeline::detail
{

/// Convenience accessors for reading AAF properties by class and property name.
class Access
{
public:
    explicit Access(const Document& doc) :
        doc_(doc),
        model_(doc.model())
    {
    }

    [[nodiscard]] auto isA(ObjectId id, std::string_view className) const -> bool
    {
        const auto* cls = model_.findClassByName(className);
        return cls != nullptr && id < doc_.objectCount() && model_.isA(doc_.object(id).classId, cls->id);
    }

    [[nodiscard]] auto value(ObjectId id, std::string_view cls, std::string_view name) const -> std::optional<Value>
    {
        return doc_.value(id, cls, name);
    }

    [[nodiscard]] auto integer(ObjectId id, std::string_view cls, std::string_view name) const -> std::optional<std::int64_t>
    {
        const auto v = value(id, cls, name);
        if (!v)
        {
            return std::nullopt;
        }
        if (v->is<std::int64_t>())
        {
            return v->as<std::int64_t>();
        }
        if (v->is<std::uint64_t>() && v->as<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return static_cast<std::int64_t>(v->as<std::uint64_t>());
        }
        return std::nullopt;
    }

    [[nodiscard]] auto string(ObjectId id, std::string_view cls, std::string_view name) const -> std::string
    {
        const auto v = value(id, cls, name);
        return v && v->is<std::string>() ? v->as<std::string>() : std::string{};
    }

    [[nodiscard]] auto rational(ObjectId id, std::string_view cls, std::string_view name) const -> std::optional<Rational>
    {
        const auto v = value(id, cls, name);
        if (!v || !v->is<Value::Record>())
        {
            return std::nullopt;
        }
        const auto& r = v->as<Value::Record>();
        if (r.values.size() != 2 || !r.values[0].is<std::int64_t>() || !r.values[1].is<std::int64_t>() || r.values[1].as<std::int64_t>() == 0)
        {
            return std::nullopt;
        }
        return Rational(r.values[0].as<std::int64_t>(), r.values[1].as<std::int64_t>());
    }

    [[nodiscard]] auto property(ObjectId id, std::string_view cls, std::string_view name) const -> const Property*
    {
        const auto* def = model_.findProperty(cls, name);
        return def == nullptr || id >= doc_.objectCount() ? nullptr : doc_.object(id).find(def->pid);
    }

    [[nodiscard]] auto child(ObjectId id, std::string_view cls, std::string_view name) const -> std::optional<ObjectId>
    {
        const auto* p = property(id, cls, name);
        const auto* s = p == nullptr ? nullptr : std::get_if<StrongRefProperty>(&p->payload);
        return s == nullptr ? std::nullopt : std::optional(s->object);
    }

    [[nodiscard]] auto children(ObjectId id, std::string_view cls, std::string_view name) const -> std::vector<ObjectId>
    {
        const auto* p = property(id, cls, name);
        if (p == nullptr)
        {
            return {};
        }
        if (const auto* v = std::get_if<StrongRefVectorProperty>(&p->payload))
        {
            return v->objects;
        }
        if (const auto* s = std::get_if<StrongRefSetProperty>(&p->payload))
        {
            return s->objects;
        }
        return {};
    }

    [[nodiscard]] auto weak(ObjectId id, std::string_view cls, std::string_view name) const -> std::optional<ObjectId>
    {
        const auto* p = property(id, cls, name);
        const auto* w = p == nullptr ? nullptr : std::get_if<WeakRefProperty>(&p->payload);
        return w == nullptr ? std::nullopt : doc_.resolveWeak(w->tag, w->key);
    }

    [[nodiscard]] auto weakKey(ObjectId id, std::string_view cls, std::string_view name) const -> std::optional<Auid>
    {
        const auto* p = property(id, cls, name);
        const auto* w = p == nullptr ? nullptr : std::get_if<WeakRefProperty>(&p->payload);
        if (w == nullptr || w->key.size() != 16)
        {
            return std::nullopt;
        }
        return Auid::fromStored(std::span<const std::byte, 16>(w->key.data(), 16), doc_.object(id).bigEndian());
    }

    [[nodiscard]] auto className(ObjectId id) const -> std::string
    {
        const auto* cls = doc_.classOf(id);
        return cls != nullptr ? cls->name : std::string("?");
    }

    [[nodiscard]] auto definitionName(std::optional<ObjectId> definition) const -> std::string
    {
        return definition ? string(*definition, "DefinitionObject", "Name") : std::string{};
    }

    const Document& doc_;
    const MetaModel& model_;
};

/// Answers "is this object a `className`?" for many objects, deciding once per distinct class.
class ClassFilter
{
public:
    ClassFilter(const Document& document, std::string_view className) :
        doc_(document),
        target_(document.model().findClassByName(className))
    {
    }

    [[nodiscard]] auto operator()(ObjectId id) const -> bool
    {
        if (target_ == nullptr || id >= doc_.objectCount())
        {
            return false;
        }
        const auto& cls = doc_.object(id).classId;
        if (const auto it = known_.find(cls); it != known_.end())
        {
            return it->second;
        }
        return known_.emplace(cls, doc_.model().isA(cls, target_->id)).first->second;
    }

private:
    const Document& doc_;
    const ClassDef* target_;
    mutable std::unordered_map<Auid, bool> known_;
};

}
