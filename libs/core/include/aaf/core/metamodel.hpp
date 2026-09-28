#pragma once

#include <aaf/core/auid.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aaf
{

enum class TypeKind : std::uint8_t {
    integer,
    enumeration,
    record,
    fixed_array,
    var_array,
    rename,
    string,
    stream,
    opaque,
    ext_enum,
    character,
    generic_character,
    indirect,
    set,
    strong_ref,
    weak_ref,
};

[[nodiscard]] auto to_string(TypeKind kind) noexcept -> std::string_view;

struct RecordField
{
    std::string name;
    Auid type;
};

struct EnumElement
{
    std::string name;
    std::int64_t value = 0;
};

struct ExtEnumElement
{
    std::string name;
    Auid value;
};

/// A type definition. Which fields are meaningful depends on `kind`.
struct TypeDef
{
    Auid id;
    std::string name;
    TypeKind kind = TypeKind::opaque;
    /// Element type (arrays, sets, strings, enums, renames) or referenced class (object references).
    Auid element;
    std::uint8_t size = 0;
    bool isSigned = false;
    std::uint32_t count = 0;
    std::vector<RecordField> fields;
    std::vector<EnumElement> enumElements;
    std::vector<ExtEnumElement> extElements;
    /// Weak references: property AUIDs of the path from the root to the target set.
    std::vector<Auid> targetPath;
};

struct PropertyDef
{
    Auid id;
    std::string name;
    /// Local identification; 0 if dynamic and not yet assigned by a file.
    std::uint16_t pid = 0;
    Auid type;
    Auid owner;
    bool optional = true;
    bool uniqueId = false;
};

struct ClassDef
{
    Auid id;
    std::string name;
    Auid parent;
    bool concrete = false;
    std::vector<Auid> properties;
};

/// Origin of a definition in a merged model.
enum class DefinitionSource : std::uint8_t {
    baseline,
    file,
    both,
};

/// The set of class, property and type definitions in effect for one file:
/// the built-in baseline merged with the file's MetaDictionary.
class MetaModel
{
public:
    /// The built-in AAF baseline model.
    [[nodiscard]] static auto baseline() -> const MetaModel&;

    [[nodiscard]] auto findClass(const Auid& id) const -> const ClassDef*;
    [[nodiscard]] auto findType(const Auid& id) const -> const TypeDef*;
    [[nodiscard]] auto findProperty(const Auid& id) const -> const PropertyDef*;
    [[nodiscard]] auto findPropertyByPid(std::uint16_t pid) const -> const PropertyDef*;
    [[nodiscard]] auto findClassByName(std::string_view name) const -> const ClassDef*;
    /// Looks up a property by owning class name and property name (not searching superclasses).
    [[nodiscard]] auto findProperty(std::string_view className, std::string_view propertyName) const -> const PropertyDef*;

    /// True if `id` is `ancestor` or derives from it.
    [[nodiscard]] auto isA(const Auid& id, const Auid& ancestor) const -> bool;
    /// All properties of a class including inherited ones, root class first.
    [[nodiscard]] auto allProperties(const Auid& classId) const -> std::vector<const PropertyDef*>;
    /// Resolves renames to the underlying type.
    [[nodiscard]] auto resolve(const Auid& typeId) const -> const TypeDef*;
    /// Stored size of a fixed-size type, or nullopt for variable-size types.
    [[nodiscard]] auto fixedSize(const Auid& typeId) const -> std::optional<std::size_t>;

    [[nodiscard]] auto classes() const noexcept -> const std::unordered_map<Auid, ClassDef>& { return classes_; }
    [[nodiscard]] auto types() const noexcept -> const std::unordered_map<Auid, TypeDef>& { return types_; }
    [[nodiscard]] auto properties() const noexcept -> const std::unordered_map<Auid, PropertyDef>& { return properties_; }
    [[nodiscard]] auto sourceOf(const Auid& id) const -> DefinitionSource;

    /// Adds or replaces definitions; used when merging a file's MetaDictionary.
    void addClass(ClassDef def, DefinitionSource source);
    void addType(TypeDef def, DefinitionSource source);
    void addProperty(PropertyDef def, DefinitionSource source);

private:
    struct NameHash
    {
        using is_transparent = void;
        auto operator()(std::string_view name) const noexcept -> std::size_t { return std::hash<std::string_view>{}(name); }
    };

    std::unordered_map<Auid, ClassDef> classes_;
    std::unordered_map<std::string, Auid, NameHash, std::equal_to<>> classesByName_;
    std::unordered_map<Auid, TypeDef> types_;
    std::unordered_map<Auid, PropertyDef> properties_;
    std::unordered_map<std::uint16_t, Auid> byPid_;
    std::unordered_map<Auid, DefinitionSource> sources_;
};

namespace ids
{

using namespace literals;

inline constexpr Auid kRootClass = "b3b398a5-1c90-11d4-8053-080036210804"_auid;
inline constexpr Auid kTypeAuid = "01030100-0000-0000-060e-2b3401040101"_auid;
inline constexpr Auid kTypeMobId = "01030200-0000-0000-060e-2b3401040101"_auid;
inline constexpr Auid kTypeBoolean = "01040100-0000-0000-060e-2b3401040101"_auid;
inline constexpr Auid kTypeCharacter = "01100100-0000-0000-060e-2b3401040101"_auid;
inline constexpr Auid kTypeString = "01100200-0000-0000-060e-2b3401040101"_auid;

inline constexpr std::uint16_t kPidRootMetaDictionary = 0x0001;
inline constexpr std::uint16_t kPidRootHeader = 0x0002;
/// InterchangeObject::ObjClass, which is implied by the storage CLSID and never stored.
inline constexpr std::uint16_t kPidObjClass = 0x0101;

}

}
