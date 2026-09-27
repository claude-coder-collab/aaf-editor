#include <aaf/edit/transaction.hpp>

#include <algorithm>

namespace aaf::edit
{

namespace
{

void addPropertyChanges(ObjectId id, const Object& before, const Object& after, std::vector<PropertyChange>& out)
{
    for (const auto& p : after.properties)
    {
        const auto* old = before.find(p.pid);
        if (old == nullptr || !(*old == p))
        {
            out.push_back({ id, p.pid });
        }
    }
    for (const auto& p : before.properties)
    {
        if (after.find(p.pid) == nullptr)
        {
            out.push_back({ id, p.pid });
        }
    }
}

}

auto Transaction::touch(ObjectId id) -> Object&
{
    if (!before_.contains(id))
    {
        before_.emplace(id, doc_.object(id));
    }
    return doc_.mutableObject(id);
}

auto Transaction::create(const Auid& classId) -> ObjectId
{
    Object o;
    o.classId = classId;
    o.byteOrder = 0x4C;
    o.formatVersion = 0x20;
    const auto id = doc_.addObject(o);
    o.id = id;
    before_.emplace(id, std::move(o));
    created_.push_back(id);
    return id;
}

auto Transaction::referencedProperties() -> std::vector<std::vector<std::uint16_t>>&
{
    if (!referencedBefore_)
    {
        referencedBefore_ = doc_.referencedProperties();
    }
    return doc_.mutableReferencedProperties();
}

auto Transaction::touched() const -> std::vector<ObjectId>
{
    std::vector<ObjectId> ids;
    ids.reserve(before_.size());
    for (const auto& [id, object] : before_)
    {
        ids.push_back(id);
    }
    return ids;
}

void Transaction::rollback()
{
    for (auto& [id, object] : before_)
    {
        doc_.mutableObject(id) = object;
    }
    if (referencedBefore_)
    {
        doc_.mutableReferencedProperties() = *referencedBefore_;
    }
    doc_.rebuildIndexes();
    before_.clear();
    referencedBefore_.reset();
}

auto Transaction::commit() && -> Journal
{
    Journal journal;
    for (auto& [id, object] : before_)
    {
        if (!(object == doc_.object(id)))
        {
            journal.after.emplace(id, doc_.object(id));
            journal.before.emplace(id, std::move(object));
        }
    }
    journal.created = std::move(created_);
    if (referencedBefore_ && *referencedBefore_ != doc_.referencedProperties())
    {
        journal.referencedPropertiesBefore = std::move(referencedBefore_);
        journal.referencedPropertiesAfter = doc_.referencedProperties();
    }
    before_.clear();
    return journal;
}

void Journal::apply(Document& document, bool redo) const
{
    for (const auto& [id, object] : redo ? after : before)
    {
        document.mutableObject(id) = object;
    }
    const auto& referenced = redo ? referencedPropertiesAfter : referencedPropertiesBefore;
    if (referenced)
    {
        document.mutableReferencedProperties() = *referenced;
    }
    document.rebuildIndexes();
}

auto Journal::changeSet() const -> ChangeSet
{
    ChangeSet changes;
    for (const auto& [id, object] : after)
    {
        changes.objects.push_back(id);
        addPropertyChanges(id, before.at(id), object, changes.properties);
    }
    for (const auto id : created)
    {
        if (after.contains(id))
        {
            changes.created.push_back(id);
        }
    }
    changes.referencedPropertiesChanged = referencedPropertiesAfter.has_value();
    return changes;
}

}
