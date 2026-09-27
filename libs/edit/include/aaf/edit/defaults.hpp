#pragma once

#include <aaf/core/metamodel.hpp>
#include <aaf/core/value.hpp>
#include <aaf/edit/transaction.hpp>

namespace aaf::edit
{

/// A reasonable initial value for a property: zero, empty, the first enumeration element, a 0/1 rational,
/// the current time for timestamps, and a new AUID or MobID for unique identifiers.
[[nodiscard]] auto defaultValue(const MetaModel& model, const PropertyDef& property) -> Result<Value>;

/// Creates a detached object with every required property set: data properties get `defaultValue`,
/// required strong references get a newly created default child (a concrete subclass is chosen if the
/// referenced class is abstract), required collections are created empty, and required weak references
/// point at the first suitable existing definition.
[[nodiscard]] auto createWithDefaults(Transaction& tx, const Auid& classId) -> Result<ObjectId>;

/// The concrete class to instantiate for a reference to `classId` (the class itself if concrete).
[[nodiscard]] auto concreteClassFor(const MetaModel& model, const Auid& classId) -> const ClassDef*;

/// Attached objects that a weak reference property of `id` may point at.
[[nodiscard]] auto weakCandidates(const Document& document, ObjectId id, std::uint16_t pid) -> std::vector<ObjectId>;

}
