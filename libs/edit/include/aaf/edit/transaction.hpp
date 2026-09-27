#pragma once

#include <aaf/core/document.hpp>

#include <map>
#include <optional>
#include <vector>

namespace aaf::edit
{

struct PropertyChange
{
    ObjectId object = kNoObject;
    std::uint16_t pid = 0;
    auto operator==(const PropertyChange&) const -> bool = default;
};

/// What a command changed, for refreshing views.
struct ChangeSet
{
    /// Objects whose state changed, including created ones.
    std::vector<ObjectId> objects;
    std::vector<ObjectId> created;
    /// Individual properties added, removed or modified.
    std::vector<PropertyChange> properties;
    bool referencedPropertiesChanged = false;
};

/// The before and after states of everything a committed transaction touched.
struct Journal
{
    std::map<ObjectId, Object> before;
    std::map<ObjectId, Object> after;
    std::vector<ObjectId> created;
    std::optional<std::vector<std::vector<std::uint16_t>>> referencedPropertiesBefore;
    std::optional<std::vector<std::vector<std::uint16_t>>> referencedPropertiesAfter;

    /// Restores the before (undo) or after (redo) state.
    void apply(Document& document, bool redo) const;
    [[nodiscard]] auto changeSet() const -> ChangeSet;
};

/// Records every object it hands out for mutation so the changes can be rolled back or journaled.
class Transaction
{
public:
    explicit Transaction(Document& document) :
        doc_(document)
    {
    }

    [[nodiscard]] auto document() const noexcept -> const Document& { return doc_; }
    /// Returns the object for mutation, snapshotting it first.
    [[nodiscard]] auto touch(ObjectId id) -> Object&;
    /// Creates a detached object of the given class.
    [[nodiscard]] auto create(const Auid& classId) -> ObjectId;
    [[nodiscard]] auto referencedProperties() -> std::vector<std::vector<std::uint16_t>>&;
    [[nodiscard]] auto touched() const -> std::vector<ObjectId>;
    [[nodiscard]] auto created() const noexcept -> const std::vector<ObjectId>& { return created_; }

    /// Permits the transaction to leave weak references unresolved (a forced delete).
    void allowDanglingReferences() noexcept { allowDangling_ = true; }
    [[nodiscard]] auto danglingReferencesAllowed() const noexcept -> bool { return allowDangling_; }

    void rollback();
    [[nodiscard]] auto commit() && -> Journal;

private:
    Document& doc_;
    std::map<ObjectId, Object> before_;
    std::vector<ObjectId> created_;
    std::optional<std::vector<std::vector<std::uint16_t>>> referencedBefore_;
    bool allowDangling_ = false;
};

}
