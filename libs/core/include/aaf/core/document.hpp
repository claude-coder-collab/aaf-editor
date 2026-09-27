#pragma once

#include <aaf/cfb/container.hpp>
#include <aaf/core/auid.hpp>
#include <aaf/core/metamodel.hpp>
#include <aaf/core/value.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace aaf
{

using ObjectId = std::uint64_t;
inline constexpr ObjectId kNoObject = ~ObjectId{ 0 };

/// Stored forms of the AAF Stored Format Specification (§1.4).
enum class StoredForm : std::uint16_t {
    data = 0x82,
    dataStream = 0x42,
    strongRef = 0x22,
    strongRefVector = 0x32,
    strongRefSet = 0x3A,
    weakRef = 0x02,
    weakRefVector = 0x12,
    weakRefSet = 0x1A,
};

struct DataProperty
{
    std::vector<std::byte> bytes;
};

struct StrongRefProperty
{
    std::u16string name;
    ObjectId object = kNoObject;
};

struct SetEntry
{
    std::uint32_t localKey = 0;
    std::uint32_t referenceCount = 0;
    std::vector<std::byte> key;
};

struct StrongRefVectorProperty
{
    std::u16string name;
    std::vector<ObjectId> objects;
    std::vector<std::uint32_t> localKeys;
    std::uint32_t firstFreeKey = 0;
    std::uint32_t lastFreeKey = 0;
};

struct StrongRefSetProperty
{
    std::u16string name;
    std::vector<ObjectId> objects;
    std::vector<SetEntry> entries;
    std::uint32_t firstFreeKey = 0;
    std::uint32_t lastFreeKey = 0;
    std::uint16_t keyPid = 0;
    std::uint8_t keySize = 0;
};

struct WeakRefProperty
{
    std::uint16_t tag = 0;
    std::uint16_t keyPid = 0;
    std::vector<std::byte> key;
};

struct WeakRefCollectionProperty
{
    std::u16string name;
    std::uint16_t tag = 0;
    std::uint16_t keyPid = 0;
    std::uint8_t keySize = 0;
    std::vector<std::vector<std::byte>> keys;
};

struct StreamProperty
{
    std::u16string name;
    std::uint8_t byteOrder = 0x4C;
    cfb::EntryId entry = cfb::kNoStream;
    std::uint64_t size = 0;
};

/// A property whose stored form is not understood; preserved verbatim.
struct UnknownProperty
{
    std::vector<std::byte> bytes;
};

using PropertyPayload = std::variant<DataProperty, StrongRefProperty, StrongRefVectorProperty, StrongRefSetProperty, WeakRefProperty, WeakRefCollectionProperty, StreamProperty, UnknownProperty>;

struct Property
{
    std::uint16_t pid = 0;
    std::uint16_t storedForm = 0;
    PropertyPayload payload;
};

struct Object
{
    ObjectId id = kNoObject;
    Auid classId;
    ObjectId parent = kNoObject;
    std::uint16_t parentPid = 0;
    std::u16string storageName;
    cfb::EntryId storage = cfb::kNoStream;
    std::uint8_t byteOrder = 0x4C;
    std::uint8_t formatVersion = 0;
    std::vector<Property> properties;
    /// Storage children not referenced by any property (preserved on save).
    std::vector<cfb::EntryId> extraEntries;

    [[nodiscard]] auto find(std::uint16_t pid) const -> const Property*;
    [[nodiscard]] auto bigEndian() const noexcept -> bool { return byteOrder == 0x42; }
};

/// A problem found while loading (non-fatal) or validating a document.
struct Diagnostic
{
    enum class Severity : std::uint8_t {
        info,
        warning,
        error,
    };
    Severity severity = Severity::warning;
    ObjectId object = kNoObject;
    std::uint16_t pid = 0;
    std::string message;
};

[[nodiscard]] auto to_string(Diagnostic::Severity severity) noexcept -> std::string_view;

/// An AAF file loaded into memory: its object graph with raw stored properties, and the
/// metamodel (baseline merged with the file's MetaDictionary) used to interpret them.
class Document
{
public:
    [[nodiscard]] static auto open(const std::filesystem::path& path) -> Result<Document>;
    [[nodiscard]] static auto load(cfb::Container container) -> Result<Document>;

    [[nodiscard]] auto container() const noexcept -> const cfb::Container& { return *container_; }
    [[nodiscard]] auto model() const noexcept -> const MetaModel& { return model_; }
    [[nodiscard]] static constexpr auto root() noexcept -> ObjectId { return 0; }
    [[nodiscard]] auto object(ObjectId id) const -> const Object& { return objects_.at(static_cast<std::size_t>(id)); }
    [[nodiscard]] auto objectCount() const noexcept -> std::size_t { return objects_.size(); }
    [[nodiscard]] auto header() const -> ObjectId;
    [[nodiscard]] auto metaDictionary() const -> ObjectId;
    [[nodiscard]] auto referencedProperties() const noexcept -> const std::vector<std::vector<std::uint16_t>>& { return referencedProperties_; }
    [[nodiscard]] auto referencedPropertiesByteOrder() const noexcept -> std::uint8_t { return referencedPropertiesByteOrder_; }
    /// Problems found while loading that did not prevent it.
    [[nodiscard]] auto loadDiagnostics() const noexcept -> const std::vector<Diagnostic>& { return loadDiagnostics_; }

    /// Class definition of an object, or nullptr if the class is unknown.
    [[nodiscard]] auto classOf(ObjectId id) const -> const ClassDef*;
    [[nodiscard]] auto propertyDef(const Property& property) const -> const PropertyDef*;
    /// Decodes a data property (stored form `data`) using its property definition.
    [[nodiscard]] auto decode(const Object& object, const Property& property) const -> Result<Value>;
    /// Finds the target object of a weak reference; nullopt if it cannot be resolved.
    [[nodiscard]] auto resolveWeak(std::uint16_t tag, std::span<const std::byte> key) const -> std::optional<ObjectId>;
    /// Reads a data property by owning class and property name, e.g. ("Mob", "Name").
    [[nodiscard]] auto value(ObjectId id, std::string_view className, std::string_view propertyName) const -> std::optional<Value>;

private:
    Document() = default;
    friend class Loader;

    std::unique_ptr<cfb::Container> container_;
    MetaModel model_;
    std::vector<Object> objects_;
    std::vector<std::vector<std::uint16_t>> referencedProperties_;
    std::uint8_t referencedPropertiesByteOrder_ = 0x4C;
    std::vector<Diagnostic> loadDiagnostics_;
    std::map<std::uint16_t, std::map<std::string, ObjectId, std::less<>>> weakIndex_;
};

/// Checks a document against its metamodel and returns all problems found.
[[nodiscard]] auto validate(const Document& document) -> std::vector<Diagnostic>;

/// Formats a key of a weak reference or set entry: 16 bytes as an AUID, 32 bytes as a MobID, else hex.
[[nodiscard]] auto formatKey(std::span<const std::byte> key, bool bigEndian = false) -> std::string;

}
