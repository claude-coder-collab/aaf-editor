#pragma once

#include <aaf/core/value.hpp>
#include <aaf/edit/transaction.hpp>

#include <cstddef>
#include <vector>

namespace aaf::edit
{

/// Sets a data property, adding it if absent. The value is encoded with the property's type.
/// Changing the unique identifier of a set element also updates the set index and every weak
/// reference to the element.
[[nodiscard]] auto setProperty(Transaction& tx, ObjectId id, std::uint16_t pid, const Value& value) -> Result<void>;
/// Removes an optional property. Strongly referenced objects become detached.
[[nodiscard]] auto removeProperty(Transaction& tx, ObjectId id, std::uint16_t pid) -> Result<void>;
/// Creates a detached object of a concrete class.
[[nodiscard]] auto createObject(Transaction& tx, const Auid& classId) -> Result<ObjectId>;
/// Sets a strong reference to a detached object; the previous child, if any, becomes detached.
[[nodiscard]] auto setStrongRef(Transaction& tx, ObjectId parent, std::uint16_t pid, ObjectId child) -> Result<void>;
/// Creates an empty strong vector or set property if it is absent.
[[nodiscard]] auto ensureCollection(Transaction& tx, ObjectId parent, std::uint16_t pid) -> Result<void>;
/// Inserts a detached object into a strong vector (at `index`) or set (index ignored), creating the property if needed.
[[nodiscard]] auto insertIntoCollection(Transaction& tx, ObjectId parent, std::uint16_t pid, std::size_t index, ObjectId child) -> Result<void>;
/// Removes the element at `index` from a strong vector or set and returns it, detached.
[[nodiscard]] auto removeFromCollection(Transaction& tx, ObjectId parent, std::uint16_t pid, std::size_t index) -> Result<ObjectId>;
/// Moves an element within a strong vector.
[[nodiscard]] auto moveInCollection(Transaction& tx, ObjectId parent, std::uint16_t pid, std::size_t from, std::size_t to) -> Result<void>;
/// Detaches an object from its parent. Unless `force`, fails if the object or a descendant is a weak-reference target.
[[nodiscard]] auto deleteObject(Transaction& tx, ObjectId id, bool force = false) -> Result<void>;
/// Sets a weak reference to an object that is an element of a strong set.
[[nodiscard]] auto setWeakRef(Transaction& tx, ObjectId id, std::uint16_t pid, ObjectId target) -> Result<void>;
/// Replaces the contents of a stream property, adding it if absent.
[[nodiscard]] auto setStreamData(Transaction& tx, ObjectId id, std::uint16_t pid, std::vector<std::byte> data) -> Result<void>;

/// The PID of a property by class and property name (searching superclasses), for convenience.
[[nodiscard]] auto pidOf(const Document& document, ObjectId id, std::string_view propertyName) -> Result<std::uint16_t>;

}
