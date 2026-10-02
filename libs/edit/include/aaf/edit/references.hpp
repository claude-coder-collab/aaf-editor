#pragma once

#include <aaf/core/value.hpp>
#include <aaf/edit/transaction.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aaf::edit
{

/// How an implicit reference finds its target.
enum class ReferenceScope : std::uint8_t {
    /// Any attached object of the target class whose key property equals the value.
    document,
    /// A slot of the mob named by the referring object's `SourceID`.
    sourceMob,
    /// A slot of the mob that contains the referring object.
    owningMob,
};

/// A data property that identifies another object by value (a MobID, an AUID or a slot ID) rather than
/// through a weak reference. The file format does not record these as references.
struct ImplicitReference
{
    std::string_view className;
    std::string_view property;
    std::string_view targetClass;
    std::string_view keyClass;
    std::string_view keyProperty;
    ReferenceScope scope = ReferenceScope::document;
};

/// Every implicit reference the editor resolves.
[[nodiscard]] auto implicitReferences() -> std::span<const ImplicitReference>;

enum class ReferenceStatus : std::uint8_t {
    /// The target is an attached object in this document.
    resolved,
    /// The value is the null identifier (for a SourceID, the end of a source chain).
    null,
    /// No object in this document has the identifier; it may live in another file.
    external,
    /// The identifier names a definition built into the AAF baseline model.
    builtin,
};

[[nodiscard]] auto to_string(ReferenceStatus status) -> std::string_view;

struct ReferenceResolution
{
    ReferenceStatus status = ReferenceStatus::external;
    ObjectId target = kNoObject;
    /// For `builtin`, the definition's name.
    std::string builtinName;
};

/// An object property that refers to another object.
struct Referrer
{
    ObjectId object = kNoObject;
    std::uint16_t pid = 0;
    /// True for a weak reference, false for an implicit reference.
    bool weak = false;
    auto operator==(const Referrer&) const -> bool = default;
};

/// Resolves implicit references and records every reference (weak and implicit) between attached objects.
/// It reflects the document when it was built and must be rebuilt after the document changes.
class ReferenceIndex
{
public:
    explicit ReferenceIndex(const Document& document);

    /// The implicit reference definition for property `pid` of object `id`, or nullptr.
    [[nodiscard]] auto definition(ObjectId id, std::uint16_t pid) const -> const ImplicitReference*;
    /// Resolves property `pid` of object `id`; nullopt if it is not an implicit reference or is absent.
    [[nodiscard]] auto resolve(ObjectId id, std::uint16_t pid) const -> std::optional<ReferenceResolution>;
    /// Attached objects that property `pid` of object `id` may refer to.
    [[nodiscard]] auto candidates(ObjectId id, std::uint16_t pid) const -> std::vector<ObjectId>;
    /// The value property `pid` of object `id` must hold to refer to `target`.
    [[nodiscard]] auto valueFor(ObjectId id, std::uint16_t pid, ObjectId target) const -> Result<Value>;
    /// References to `target`, weak and implicit, from attached objects.
    [[nodiscard]] auto referrers(ObjectId target) const -> std::span<const Referrer>;
    /// Implicit references to `target` that identify it by its property `keyPid`.
    [[nodiscard]] auto referrersByKey(ObjectId target, std::uint16_t keyPid) const -> std::vector<Referrer>;

private:
    struct Spec
    {
        const ImplicitReference* reference = nullptr;
        Auid ownerClass;
        Auid targetClass;
        std::uint16_t keyPid = 0;
        /// Key prefix of the targets, for document-scoped references.
        std::string prefix;
    };

    [[nodiscard]] auto specFor(ObjectId id, std::uint16_t pid) const -> const Spec*;
    [[nodiscard]] auto scopeMob(ObjectId id, const Spec& spec) const -> std::optional<ObjectId>;
    void collectPrefixed(const std::string& prefix, std::vector<ObjectId>& out) const;
    [[nodiscard]] auto isA(ObjectId id, const Auid& classId) const -> bool;
    [[nodiscard]] auto membership(ObjectId id) const -> std::uint64_t;
    [[nodiscard]] auto bitOf(const Auid& classId) const -> std::uint64_t;
    void addKey(std::string prefix, const Value& value, ObjectId target);

    const Document& doc_;
    std::unordered_multimap<std::uint16_t, Spec> specs_;
    std::unordered_map<std::string, ObjectId> keyed_;
    std::unordered_map<std::string, std::vector<ObjectId>> groups_;
    std::unordered_map<ObjectId, std::vector<Referrer>> incoming_;
    Auid mobClass_;
    std::uint16_t sourceIdPid_ = 0;
    std::vector<Auid> classes_;
    mutable std::unordered_map<Auid, std::uint64_t> memberships_;
};

/// Sets implicit reference property `pid` of object `id` to identify `target`, which must be a candidate.
[[nodiscard]] auto setReference(Transaction& tx, ObjectId id, std::uint16_t pid, ObjectId target) -> Result<void>;

/// Sets a data property like `setProperty`. If the property is the identifier implicit references use to
/// find the object (a MobID, a definition's Identification or a SlotID), those references are rewritten
/// to the new value.
[[nodiscard]] auto setIdentifier(Transaction& tx, ObjectId id, std::uint16_t pid, const Value& value) -> Result<void>;

/// Implicit references from outside the subtree rooted at `id` that resolve to an object inside it.
[[nodiscard]] auto incomingImplicitReferences(const Document& document, ObjectId id) -> std::vector<Referrer>;
/// As above, using an index already built for the document's current state.
[[nodiscard]] auto incomingImplicitReferences(const Document& document, const ReferenceIndex& index, ObjectId id) -> std::vector<Referrer>;

}
