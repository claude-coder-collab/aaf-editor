#include <aaf/edit/session.hpp>

#include <format>
#include <limits>

namespace aaf::edit
{

namespace
{

constexpr std::size_t kNeverSaved = std::numeric_limits<std::size_t>::max();

}

Session::Session(Document document) :
    document_(std::move(document)),
    dangling_(danglingCount())
{
}

auto Session::open(const std::filesystem::path& path) -> Result<Session>
{
    auto doc = Document::open(path);
    if (!doc)
    {
        return std::unexpected(doc.error());
    }
    return Session(std::move(*doc));
}

auto Session::danglingCount() const -> std::size_t
{
    std::size_t count = 0;
    auto check = [&](const Object& o, std::uint16_t tag, std::span<const std::byte> key) -> void {
        if (!document_.resolveWeak(tag, key) && !isKnownDefinitionKey(document_.model(), key, o.bigEndian()))
        {
            ++count;
        }
    };
    for (std::size_t i = 0; i < document_.objectCount(); ++i)
    {
        const auto& o = document_.object(i);
        if (o.properties.empty() || !document_.isAttached(i))
        {
            continue;
        }
        for (const auto& p : o.properties)
        {
            if (const auto* w = std::get_if<WeakRefProperty>(&p.payload))
            {
                check(o, w->tag, w->key);
            }
            else if (const auto* c = std::get_if<WeakRefCollectionProperty>(&p.payload))
            {
                for (const auto& key : c->keys)
                {
                    check(o, c->tag, key);
                }
            }
        }
    }
    return count;
}

auto Session::checkCommitted(const Transaction& tx) const -> Result<void>
{
    for (const auto id : tx.touched())
    {
        if (!document_.isAttached(id))
        {
            continue;
        }
        for (const auto& d : validateObject(document_, id))
        {
            if (d.severity == Diagnostic::Severity::error)
            {
                return fail(Errc::invalid_argument, d.message);
            }
        }
    }
    if (!tx.danglingReferencesAllowed())
    {
        if (const auto now = danglingCount(); now > dangling_)
        {
            return fail(Errc::invalid_argument, std::format("the change would leave {} weak reference(s) unresolved", now - dangling_));
        }
    }
    return {};
}

void Session::notify(const ChangeSet& changes) const
{
    if (listener_)
    {
        listener_(changes);
    }
}

auto Session::execute(std::string description, const Command& command) -> Result<ChangeSet>
{
    Transaction tx(document_);
    if (auto r = command(tx); !r)
    {
        tx.rollback();
        return std::unexpected(r.error());
    }
    document_.rebuildIndexes();
    if (auto r = checkCommitted(tx); !r)
    {
        tx.rollback();
        return std::unexpected(r.error());
    }
    auto journal = std::move(tx).commit();
    if (journal.after.empty() && !journal.referencedPropertiesAfter)
    {
        return ChangeSet{};
    }
    history_.resize(position_);
    if (savedPosition_ > position_)
    {
        savedPosition_ = kNeverSaved;
    }
    auto changes = journal.changeSet();
    history_.push_back(Step{ std::move(description), std::move(journal) });
    ++position_;
    dangling_ = danglingCount();
    notify(changes);
    return changes;
}

auto Session::undo() -> Result<ChangeSet>
{
    if (!canUndo())
    {
        return fail(Errc::invalid_argument, "nothing to undo");
    }
    const auto& journal = history_[--position_].journal;
    journal.apply(document_, false);
    dangling_ = danglingCount();
    auto changes = journal.changeSet();
    notify(changes);
    return changes;
}

auto Session::redo() -> Result<ChangeSet>
{
    if (!canRedo())
    {
        return fail(Errc::invalid_argument, "nothing to redo");
    }
    const auto& journal = history_[position_++].journal;
    journal.apply(document_, true);
    dangling_ = danglingCount();
    auto changes = journal.changeSet();
    notify(changes);
    return changes;
}

auto Session::history() const -> std::vector<std::string>
{
    std::vector<std::string> out;
    out.reserve(history_.size());
    for (const auto& step : history_)
    {
        out.push_back(step.description);
    }
    return out;
}

auto Session::save(const std::filesystem::path& path, const WriteOptions& options) -> Result<void>
{
    if (auto r = aaf::save(document_, path, options); !r)
    {
        return r;
    }
    savedPosition_ = position_;
    return {};
}

}
