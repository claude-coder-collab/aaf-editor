#include <aaf/core/document.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <set>

namespace aaf
{

namespace
{

constexpr std::uint64_t kMaxPropertiesStream = std::uint64_t{ 64 } << 20;
constexpr std::uint64_t kMaxIndexStream = std::uint64_t{ 256 } << 20;
constexpr std::uint8_t kLittleEndian = 0x4C;
constexpr std::uint8_t kBigEndian = 0x42;

class Reader
{
public:
    Reader(std::span<const std::byte> data, bool bigEndian) :
        data_(data),
        bigEndian_(bigEndian)
    {
    }

    [[nodiscard]] auto remaining() const noexcept -> std::size_t { return data_.size() - pos_; }
    [[nodiscard]] auto bytes(std::size_t n) -> Result<std::span<const std::byte>>
    {
        if (n > remaining())
        {
            return fail(Errc::format, "truncated structure");
        }
        auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }
    template <std::unsigned_integral T>
    [[nodiscard]] auto read() -> Result<T>
    {
        auto b = bytes(sizeof(T));
        if (!b)
        {
            return std::unexpected(b.error());
        }
        T v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            const std::size_t index = bigEndian_ ? i : sizeof(T) - 1 - i;
            v = static_cast<T>((v << 8U) | std::to_integer<T>((*b)[index]));
        }
        return v;
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool bigEndian_;
};

auto decodeName(std::span<const std::byte> b, bool bigEndian) -> std::u16string
{
    std::u16string out;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2)
    {
        const auto hi = std::to_integer<unsigned>(b[bigEndian ? i : i + 1]);
        const auto lo = std::to_integer<unsigned>(b[bigEndian ? i + 1 : i]);
        const auto c = static_cast<char16_t>((hi << 8U) | lo);
        if (c == 0)
        {
            break;
        }
        out.push_back(c);
    }
    return out;
}

auto elementName(std::u16string_view base, std::uint32_t localKey) -> std::u16string
{
    std::u16string name(base);
    name += u'{';
    for (const char c : std::format("{:x}", localKey))
    {
        name.push_back(static_cast<char16_t>(c));
    }
    name += u'}';
    return name;
}

auto keyString(std::span<const std::byte> key) -> std::string
{
    std::string out(key.size(), '\0');
    std::ranges::transform(key, out.begin(), [](std::byte b) -> char { return static_cast<char>(b); });
    return out;
}

auto typeKindOf(std::string_view typeDefinitionClass) -> TypeKind
{
    static constexpr std::array<std::pair<std::string_view, TypeKind>, 8> kKinds = { {
        { "TypeDefinitionVariableArray", TypeKind::var_array },
        { "TypeDefinitionSet", TypeKind::set },
        { "TypeDefinitionString", TypeKind::string },
        { "TypeDefinitionRename", TypeKind::rename },
        { "TypeDefinitionStream", TypeKind::stream },
        { "TypeDefinitionIndirect", TypeKind::indirect },
        { "TypeDefinitionOpaque", TypeKind::opaque },
        { "TypeDefinitionCharacter", TypeKind::character },
    } };
    for (const auto& [name, kind] : kKinds)
    {
        if (name == typeDefinitionClass)
        {
            return kind;
        }
    }
    return TypeKind::opaque;
}

auto auidOf(const Property* p, bool bigEndian) -> std::optional<Auid>
{
    if (p == nullptr)
    {
        return std::nullopt;
    }
    if (const auto* d = std::get_if<DataProperty>(&p->payload); d != nullptr && d->bytes.size() == 16)
    {
        return Auid::fromStored(std::span<const std::byte, 16>(d->bytes.data(), 16), bigEndian);
    }
    if (const auto* w = std::get_if<WeakRefProperty>(&p->payload); w != nullptr && w->key.size() == 16)
    {
        return Auid::fromStored(std::span<const std::byte, 16>(w->key.data(), 16), bigEndian);
    }
    return std::nullopt;
}

}

class Loader
{
public:
    Loader(Document& doc) :
        doc_(doc),
        c_(*doc.container_)
    {
    }

    auto run() -> Result<void>
    {
        if (auto r = loadObjects(); !r)
        {
            return r;
        }
        if (auto r = loadReferencedProperties(); !r)
        {
            return r;
        }
        buildModel();
        buildWeakIndex();
        return {};
    }

private:
    void warn(ObjectId object, std::uint16_t pid, std::string message)
    {
        doc_.loadDiagnostics_.push_back({ Diagnostic::Severity::warning, object, pid, std::move(message) });
    }

    auto readStream(cfb::EntryId storage, std::u16string_view name, std::uint64_t limit) -> Result<std::vector<std::byte>>
    {
        const auto id = c_.find(storage, name);
        if (!id || !c_.entry(*id).isStream())
        {
            return fail(Errc::format, std::format("missing stream '{}'", cfb::toUtf8(name)));
        }
        auto reader = c_.openStream(*id);
        if (!reader)
        {
            return std::unexpected(reader.error());
        }
        return reader->readAll(limit);
    }

    auto newObject(cfb::EntryId storage, ObjectId parent, std::uint16_t parentPid) -> Result<ObjectId>
    {
        if (!claimed_.insert(storage).second)
        {
            return fail(Errc::format, std::format("storage '{}' is referenced more than once", cfb::toUtf8(c_.entry(storage).name)));
        }
        Object o;
        o.id = doc_.objects_.size();
        o.storage = storage;
        o.storageName = c_.entry(storage).name;
        o.classId = Auid{ c_.entry(storage).clsid.bytes };
        o.parent = parent;
        o.parentPid = parentPid;
        doc_.objects_.push_back(std::move(o));
        pending_.push_back(doc_.objects_.back().id);
        return doc_.objects_.back().id;
    }

    auto childStorage(ObjectId owner, std::u16string_view name) -> Result<cfb::EntryId>
    {
        const auto storage = doc_.objects_[static_cast<std::size_t>(owner)].storage;
        const auto id = c_.find(storage, name);
        if (!id || !c_.entry(*id).isStorage())
        {
            return fail(Errc::format, std::format("missing object storage '{}'", cfb::toUtf8(name)));
        }
        referenced_[owner].insert(*id);
        return *id;
    }

    auto readIndex(ObjectId owner, std::u16string_view name) -> Result<std::vector<std::byte>>
    {
        std::u16string indexName(name);
        indexName += u" index";
        const auto storage = doc_.objects_[static_cast<std::size_t>(owner)].storage;
        if (const auto id = c_.find(storage, indexName))
        {
            referenced_[owner].insert(*id);
        }
        return readStream(storage, indexName, kMaxIndexStream);
    }

    auto loadObjects() -> Result<void>
    {
        if (auto r = newObject(0, kNoObject, 0); !r)
        {
            return std::unexpected(r.error());
        }
        while (!pending_.empty())
        {
            const ObjectId id = pending_.front();
            pending_.pop_front();
            if (auto r = loadProperties(id); !r)
            {
                const auto& o = doc_.objects_[static_cast<std::size_t>(id)];
                auto error = r.error();
                error.message = std::format("object '{}': {}", cfb::toUtf8(o.storageName), error.message);
                return std::unexpected(std::move(error));
            }
        }
        for (auto& o : doc_.objects_)
        {
            const auto& refs = referenced_[o.id];
            for (const auto child : c_.entry(o.storage).children)
            {
                if (c_.entry(child).name != u"properties" && !refs.contains(child))
                {
                    o.extraEntries.push_back(child);
                }
            }
        }
        return {};
    }

    auto loadProperties(ObjectId id) -> Result<void>
    {
        const auto storage = doc_.objects_[static_cast<std::size_t>(id)].storage;
        if (const auto p = c_.find(storage, u"properties"))
        {
            referenced_[id].insert(*p);
        }
        auto data = readStream(storage, u"properties", kMaxPropertiesStream);
        if (!data)
        {
            return std::unexpected(data.error());
        }
        if (data->size() < 4)
        {
            return fail(Errc::format, "properties stream is too short");
        }
        const auto byteOrder = std::to_integer<std::uint8_t>((*data)[0]);
        if (byteOrder != kLittleEndian && byteOrder != kBigEndian)
        {
            return fail(Errc::format, std::format("invalid byte order {:#04x}", byteOrder));
        }
        const bool be = byteOrder == kBigEndian;
        Reader header(*data, be);
        if (!header.bytes(2))
        {
            return fail(Errc::format, "properties stream is too short");
        }
        const auto count = header.read<std::uint16_t>().value();
        struct Entry
        {
            std::uint16_t pid;
            std::uint16_t form;
            std::uint16_t length;
        };
        std::vector<Entry> entries;
        entries.reserve(count);
        for (std::uint16_t i = 0; i < count; ++i)
        {
            auto pid = header.read<std::uint16_t>();
            auto form = header.read<std::uint16_t>();
            auto length = header.read<std::uint16_t>();
            if (!pid || !form || !length)
            {
                return fail(Errc::format, "truncated property index");
            }
            entries.push_back({ *pid, *form, *length });
        }

        {
            auto& o = doc_.objects_[static_cast<std::size_t>(id)];
            o.byteOrder = byteOrder;
            o.formatVersion = std::to_integer<std::uint8_t>((*data)[1]);
        }
        std::vector<Property> properties;
        properties.reserve(count);
        for (const auto& e : entries)
        {
            auto value = header.bytes(e.length);
            if (!value)
            {
                return fail(Errc::format, std::format("property {:#06x} value extends past end of stream", e.pid));
            }
            auto payload = parsePayload(id, e.pid, e.form, *value, be);
            if (!payload)
            {
                auto error = payload.error();
                error.message = std::format("property {:#06x}: {}", e.pid, error.message);
                return std::unexpected(std::move(error));
            }
            properties.push_back(Property{ e.pid, e.form, std::move(*payload) });
        }
        doc_.objects_[static_cast<std::size_t>(id)].properties = std::move(properties);
        return {};
    }

    auto parsePayload(ObjectId owner, std::uint16_t pid, std::uint16_t form, std::span<const std::byte> value, bool be) -> Result<PropertyPayload>
    {
        switch (static_cast<StoredForm>(form))
        {
            case StoredForm::data:
                return DataProperty{ { value.begin(), value.end() } };
            case StoredForm::strongRef:
            {
                StrongRefProperty p{ decodeName(value, be), kNoObject };
                auto storage = childStorage(owner, p.name);
                if (!storage)
                {
                    return std::unexpected(storage.error());
                }
                auto child = newObject(*storage, owner, pid);
                if (!child)
                {
                    return std::unexpected(child.error());
                }
                p.object = *child;
                return p;
            }
            case StoredForm::strongRefVector:
                return parseStrongVector(owner, pid, decodeName(value, be), be);
            case StoredForm::strongRefSet:
                return parseStrongSet(owner, pid, decodeName(value, be), be);
            case StoredForm::weakRef:
            {
                Reader r(value, be);
                auto tag = r.read<std::uint16_t>();
                auto keyPid = r.read<std::uint16_t>();
                auto keySize = r.read<std::uint8_t>();
                if (!tag || !keyPid || !keySize)
                {
                    return fail(Errc::format, "truncated weak reference");
                }
                auto key = r.bytes(*keySize);
                if (!key)
                {
                    return std::unexpected(key.error());
                }
                return WeakRefProperty{ *tag, *keyPid, { key->begin(), key->end() } };
            }
            case StoredForm::weakRefVector:
            case StoredForm::weakRefSet:
                return parseWeakCollection(owner, decodeName(value, be), be);
            case StoredForm::dataStream:
            {
                if (value.empty())
                {
                    return fail(Errc::format, "empty data stream reference");
                }
                StreamProperty p;
                p.byteOrder = std::to_integer<std::uint8_t>(value[0]);
                p.name = decodeName(value.subspan(1), be);
                const auto storage = doc_.objects_[static_cast<std::size_t>(owner)].storage;
                if (const auto entry = c_.find(storage, p.name); entry && c_.entry(*entry).isStream())
                {
                    p.entry = *entry;
                    p.size = c_.entry(*entry).size;
                    referenced_[owner].insert(*entry);
                }
                else
                {
                    warn(owner, pid, std::format("data stream '{}' is missing", cfb::toUtf8(p.name)));
                }
                return p;
            }
        }
        return UnknownProperty{ { value.begin(), value.end() } };
    }

    auto parseStrongVector(ObjectId owner, std::uint16_t pid, std::u16string name, bool be) -> Result<PropertyPayload>
    {
        auto index = readIndex(owner, name);
        if (!index)
        {
            return std::unexpected(index.error());
        }
        Reader r(*index, be);
        auto count = r.read<std::uint32_t>();
        auto firstFree = r.read<std::uint32_t>();
        auto lastFree = r.read<std::uint32_t>();
        if (!count || !firstFree || !lastFree || std::uint64_t{ *count } * 4 > r.remaining())
        {
            return fail(Errc::format, "malformed vector index");
        }
        StrongRefVectorProperty p{ std::move(name), {}, {}, *firstFree, *lastFree };
        for (std::uint32_t i = 0; i < *count; ++i)
        {
            const auto key = r.read<std::uint32_t>().value();
            auto storage = childStorage(owner, elementName(p.name, key));
            if (!storage)
            {
                return std::unexpected(storage.error());
            }
            auto child = newObject(*storage, owner, pid);
            if (!child)
            {
                return std::unexpected(child.error());
            }
            p.localKeys.push_back(key);
            p.objects.push_back(*child);
        }
        return p;
    }

    auto parseStrongSet(ObjectId owner, std::uint16_t pid, std::u16string name, bool be) -> Result<PropertyPayload>
    {
        auto index = readIndex(owner, name);
        if (!index)
        {
            return std::unexpected(index.error());
        }
        Reader r(*index, be);
        auto count = r.read<std::uint32_t>();
        auto firstFree = r.read<std::uint32_t>();
        auto lastFree = r.read<std::uint32_t>();
        auto keyPid = r.read<std::uint16_t>();
        auto keySize = r.read<std::uint8_t>();
        if (!count || !firstFree || !lastFree || !keyPid || !keySize || std::uint64_t{ *count } * (8 + *keySize) > r.remaining())
        {
            return fail(Errc::format, "malformed set index");
        }
        StrongRefSetProperty p{ std::move(name), {}, {}, *firstFree, *lastFree, *keyPid, *keySize };
        for (std::uint32_t i = 0; i < *count; ++i)
        {
            SetEntry entry;
            entry.localKey = r.read<std::uint32_t>().value();
            entry.referenceCount = r.read<std::uint32_t>().value();
            const auto key = r.bytes(*keySize).value();
            entry.key.assign(key.begin(), key.end());
            auto storage = childStorage(owner, elementName(p.name, entry.localKey));
            if (!storage)
            {
                return std::unexpected(storage.error());
            }
            auto child = newObject(*storage, owner, pid);
            if (!child)
            {
                return std::unexpected(child.error());
            }
            p.entries.push_back(std::move(entry));
            p.objects.push_back(*child);
        }
        return p;
    }

    auto parseWeakCollection(ObjectId owner, std::u16string name, bool be) -> Result<PropertyPayload>
    {
        auto index = readIndex(owner, name);
        if (!index)
        {
            return std::unexpected(index.error());
        }
        Reader r(*index, be);
        auto count = r.read<std::uint32_t>();
        auto tag = r.read<std::uint16_t>();
        auto keyPid = r.read<std::uint16_t>();
        auto keySize = r.read<std::uint8_t>();
        if (!count || !tag || !keyPid || !keySize || std::uint64_t{ *count } * *keySize > r.remaining())
        {
            return fail(Errc::format, "malformed weak reference index");
        }
        WeakRefCollectionProperty p{ std::move(name), *tag, *keyPid, *keySize, {} };
        for (std::uint32_t i = 0; i < *count; ++i)
        {
            const auto key = r.bytes(*keySize).value();
            p.keys.emplace_back(key.begin(), key.end());
        }
        return p;
    }

    auto loadReferencedProperties() -> Result<void>
    {
        const auto id = c_.find(0, u"referenced properties");
        if (!id)
        {
            return {};
        }
        referenced_[0].insert(*id);
        std::erase(doc_.objects_[0].extraEntries, *id);
        auto data = readStream(0, u"referenced properties", kMaxIndexStream);
        if (!data)
        {
            return std::unexpected(data.error());
        }
        if (data->empty())
        {
            return fail(Errc::format, "empty referenced properties stream");
        }
        const auto byteOrder = std::to_integer<std::uint8_t>((*data)[0]);
        Reader r{ std::span<const std::byte>(*data).subspan(1), byteOrder == kBigEndian };
        auto pathCount = r.read<std::uint16_t>();
        auto pidCount = r.read<std::uint32_t>();
        if (!pathCount || !pidCount || std::uint64_t{ *pidCount } * 2 > r.remaining())
        {
            return fail(Errc::format, "malformed referenced properties stream");
        }
        doc_.referencedPropertiesByteOrder_ = byteOrder;
        std::vector<std::uint16_t> path;
        for (std::uint32_t i = 0; i < *pidCount; ++i)
        {
            const auto pid = r.read<std::uint16_t>().value();
            if (pid == 0)
            {
                doc_.referencedProperties_.push_back(std::move(path));
                path.clear();
            }
            else
            {
                path.push_back(pid);
            }
        }
        if (doc_.referencedProperties_.size() != *pathCount)
        {
            warn(0, 0, std::format("referenced properties declares {} paths but contains {}", *pathCount, doc_.referencedProperties_.size()));
        }
        return {};
    }

    [[nodiscard]] static auto prop(const Object& o, std::string_view cls, std::string_view name) -> const Property*
    {
        const auto* def = MetaModel::baseline().findProperty(cls, name);
        return def == nullptr ? nullptr : o.find(def->pid);
    }

    [[nodiscard]] static auto decodeAs(const Object& o, const Property* p, const Auid& type) -> std::optional<Value>
    {
        const auto* d = p == nullptr ? nullptr : std::get_if<DataProperty>(&p->payload);
        if (d == nullptr)
        {
            return std::nullopt;
        }
        auto v = decodeValue(MetaModel::baseline(), type, d->bytes, o.bigEndian());
        return v ? std::optional(std::move(*v)) : std::nullopt;
    }

    [[nodiscard]] static auto stringOf(const Object& o, std::string_view cls, std::string_view name) -> std::string
    {
        auto v = decodeAs(o, prop(o, cls, name), ids::kTypeString);
        return v && v->is<std::string>() ? v->as<std::string>() : std::string{};
    }

    [[nodiscard]] static auto boolOf(const Object& o, std::string_view cls, std::string_view name, bool fallback) -> bool
    {
        auto v = decodeAs(o, prop(o, cls, name), ids::kTypeBoolean);
        return v && v->is<bool>() ? v->as<bool>() : fallback;
    }

    [[nodiscard]] static auto uintOf(const Object& o, const Property* p) -> std::optional<std::uint64_t>
    {
        const auto* d = p == nullptr ? nullptr : std::get_if<DataProperty>(&p->payload);
        if (d == nullptr || d->bytes.empty() || d->bytes.size() > 8)
        {
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < d->bytes.size(); ++i)
        {
            const std::size_t index = o.bigEndian() ? i : d->bytes.size() - 1 - i;
            v = (v << 8U) | std::to_integer<std::uint64_t>(d->bytes[index]);
        }
        return v;
    }

    [[nodiscard]] static auto arrayOf(const Object& o, std::string_view cls, std::string_view name) -> Value::Array
    {
        const auto* def = MetaModel::baseline().findProperty(cls, name);
        if (def == nullptr)
        {
            return {};
        }
        auto v = decodeAs(o, o.find(def->pid), def->type);
        return v && v->is<Value::Array>() ? v->as<Value::Array>() : Value::Array{};
    }

    [[nodiscard]] static auto setChildren(const Object& o, std::string_view cls, std::string_view name) -> std::vector<ObjectId>
    {
        const auto* p = prop(o, cls, name);
        if (p == nullptr)
        {
            return {};
        }
        if (const auto* s = std::get_if<StrongRefSetProperty>(&p->payload))
        {
            return s->objects;
        }
        if (const auto* v = std::get_if<StrongRefVectorProperty>(&p->payload))
        {
            return v->objects;
        }
        return {};
    }

    void buildModel()
    {
        doc_.model_ = MetaModel::baseline();
        const auto metaDictionary = doc_.metaDictionary();
        if (metaDictionary == kNoObject)
        {
            warn(0, 0, "file has no MetaDictionary");
            return;
        }
        const auto& md = doc_.object(metaDictionary);
        for (const auto classObject : setChildren(md, "MetaDictionary", "ClassDefinitions"))
        {
            mergeClass(doc_.object(classObject));
        }
        for (const auto typeObject : setChildren(md, "MetaDictionary", "TypeDefinitions"))
        {
            mergeType(doc_.object(typeObject));
        }
    }

    void mergeClass(const Object& o)
    {
        const auto id = auidOf(prop(o, "MetaDefinition", "Identification"), o.bigEndian());
        if (!id)
        {
            warn(o.id, 0, "class definition without Identification");
            return;
        }
        auto& model = doc_.model_;
        const auto* existing = model.findClass(*id);
        ClassDef def = existing != nullptr ? *existing : ClassDef{};
        def.id = *id;
        if (auto name = stringOf(o, "MetaDefinition", "Name"); !name.empty())
        {
            def.name = std::move(name);
        }
        if (auto parent = auidOf(prop(o, "ClassDefinition", "ParentClass"), o.bigEndian()))
        {
            def.parent = *parent == *id ? Auid{} : *parent;
        }
        def.concrete = boolOf(o, "ClassDefinition", "IsConcrete", def.concrete);
        for (const auto propertyObject : setChildren(o, "ClassDefinition", "Properties"))
        {
            const auto& po = doc_.object(propertyObject);
            const auto pidValue = uintOf(po, prop(po, "PropertyDefinition", "LocalIdentification"));
            const auto propertyId = auidOf(prop(po, "MetaDefinition", "Identification"), po.bigEndian());
            if (!propertyId || !pidValue)
            {
                warn(po.id, 0, "property definition without Identification or LocalIdentification");
                continue;
            }
            const auto* known = model.findProperty(*propertyId);
            PropertyDef p = known != nullptr ? *known : PropertyDef{};
            p.id = *propertyId;
            p.owner = def.id;
            p.pid = static_cast<std::uint16_t>(*pidValue);
            if (auto name = stringOf(po, "MetaDefinition", "Name"); !name.empty())
            {
                p.name = std::move(name);
            }
            if (auto type = auidOf(prop(po, "PropertyDefinition", "Type"), po.bigEndian()))
            {
                p.type = *type;
            }
            p.optional = boolOf(po, "PropertyDefinition", "IsOptional", p.optional);
            p.uniqueId = boolOf(po, "PropertyDefinition", "IsUniqueIdentifier", p.uniqueId);
            if (known != nullptr && (known->pid != 0 && known->pid != p.pid))
            {
                warn(po.id, 0, std::format("property {} redefines PID {:#06x} as {:#06x}", p.name, known->pid, p.pid));
            }
            if (std::ranges::find(def.properties, p.id) == def.properties.end())
            {
                def.properties.push_back(p.id);
            }
            model.addProperty(std::move(p), known != nullptr ? DefinitionSource::both : DefinitionSource::file);
        }
        model.addClass(std::move(def), existing != nullptr ? DefinitionSource::both : DefinitionSource::file);
    }

    void mergeType(const Object& o)
    {
        const auto id = auidOf(prop(o, "MetaDefinition", "Identification"), o.bigEndian());
        const auto* cls = MetaModel::baseline().findClass(o.classId);
        if (!id || cls == nullptr)
        {
            warn(o.id, 0, "type definition without Identification or with an unknown class");
            return;
        }
        auto& model = doc_.model_;
        const auto* existing = model.findType(*id);
        TypeDef t;
        t.id = *id;
        t.name = stringOf(o, "MetaDefinition", "Name");
        const bool be = o.bigEndian();
        auto weak = [&](std::string_view c, std::string_view n) -> aaf::Auid { return auidOf(prop(o, c, n), be).value_or(Auid{}); };
        const auto& name = cls->name;
        if (name == "TypeDefinitionInteger")
        {
            t.kind = TypeKind::integer;
            t.size = static_cast<std::uint8_t>(uintOf(o, prop(o, name, "Size")).value_or(0));
            t.isSigned = boolOf(o, name, "IsSigned", false);
        }
        else if (name == "TypeDefinitionStrongObjectReference")
        {
            t.kind = TypeKind::strong_ref;
            t.element = weak(name, "ReferencedType");
        }
        else if (name == "TypeDefinitionWeakObjectReference")
        {
            t.kind = TypeKind::weak_ref;
            t.element = weak(name, "ReferencedType");
            for (const auto& v : arrayOf(o, name, "TargetSet"))
            {
                if (v.is<Auid>())
                {
                    t.targetPath.push_back(v.as<Auid>());
                }
            }
        }
        else if (name == "TypeDefinitionEnumeration")
        {
            t.kind = TypeKind::enumeration;
            t.element = weak(name, "ElementType");
            const auto names = arrayOf(o, name, "ElementNames");
            const auto values = arrayOf(o, name, "ElementValues");
            for (std::size_t i = 0; i < names.size() && i < values.size(); ++i)
            {
                if (names[i].is<std::string>() && values[i].is<std::int64_t>())
                {
                    t.enumElements.push_back({ names[i].as<std::string>(), values[i].as<std::int64_t>() });
                }
            }
        }
        else if (name == "TypeDefinitionFixedArray")
        {
            t.kind = TypeKind::fixed_array;
            t.element = weak(name, "ElementType");
            t.count = static_cast<std::uint32_t>(uintOf(o, prop(o, name, "ElementCount")).value_or(0));
        }
        else if (name == "TypeDefinitionVariableArray" || name == "TypeDefinitionSet" || name == "TypeDefinitionString" || name == "TypeDefinitionRename")
        {
            t.kind = typeKindOf(name);
            t.element = weak(name, t.kind == TypeKind::rename ? "RenamedType" : "ElementType");
        }
        else if (name == "TypeDefinitionRecord")
        {
            t.kind = TypeKind::record;
            const auto names = arrayOf(o, name, "MemberNames");
            const auto* typesProp = prop(o, name, "MemberTypes");
            const auto* types = typesProp == nullptr ? nullptr : std::get_if<WeakRefCollectionProperty>(&typesProp->payload);
            for (std::size_t i = 0; types != nullptr && i < names.size() && i < types->keys.size(); ++i)
            {
                if (names[i].is<std::string>() && types->keys[i].size() == 16)
                {
                    t.fields.push_back({ names[i].as<std::string>(), Auid::fromStored(std::span<const std::byte, 16>(types->keys[i].data(), 16), be) });
                }
            }
        }
        else if (name == "TypeDefinitionExtendibleEnumeration")
        {
            t.kind = TypeKind::ext_enum;
            const auto names = arrayOf(o, name, "ElementNames");
            const auto values = arrayOf(o, name, "ElementValues");
            for (std::size_t i = 0; i < names.size() && i < values.size(); ++i)
            {
                if (names[i].is<std::string>() && values[i].is<Auid>())
                {
                    t.extElements.push_back({ names[i].as<std::string>(), values[i].as<Auid>() });
                }
            }
        }
        else if (name == "TypeDefinitionGenericCharacter")
        {
            t.kind = TypeKind::generic_character;
            const auto* sizeDef = model.findProperty(cls->properties.empty() ? Auid{} : cls->properties.front());
            t.size = static_cast<std::uint8_t>(sizeDef == nullptr ? 0 : uintOf(o, o.find(sizeDef->pid)).value_or(0));
        }
        else
        {
            t.kind = typeKindOf(name);
        }
        if (existing != nullptr)
        {
            if (existing->kind != t.kind)
            {
                warn(o.id, 0, std::format("type {} redefined with a different kind", t.name));
            }
            if (t.kind == TypeKind::ext_enum)
            {
                for (const auto& e : existing->extElements)
                {
                    if (std::ranges::none_of(t.extElements, [&e](const auto& x) -> auto { return x.value == e.value; }))
                    {
                        t.extElements.push_back(e);
                    }
                }
            }
            if (t.targetPath.empty())
            {
                t.targetPath = existing->targetPath;
            }
        }
        model.addType(std::move(t), existing != nullptr ? DefinitionSource::both : DefinitionSource::file);
    }

    void buildWeakIndex()
    {
        for (std::size_t tag = 0; tag < doc_.referencedProperties_.size(); ++tag)
        {
            const auto& path = doc_.referencedProperties_[tag];
            ObjectId current = Document::root();
            const Property* target = nullptr;
            for (std::size_t i = 0; i < path.size() && current != kNoObject; ++i)
            {
                target = doc_.object(current).find(path[i]);
                if (target == nullptr)
                {
                    current = kNoObject;
                    break;
                }
                if (i + 1 < path.size())
                {
                    const auto* strong = std::get_if<StrongRefProperty>(&target->payload);
                    current = strong == nullptr ? kNoObject : strong->object;
                }
            }
            if (current == kNoObject || target == nullptr)
            {
                continue;
            }
            auto& index = doc_.weakIndex_[static_cast<std::uint16_t>(tag)];
            if (const auto* set = std::get_if<StrongRefSetProperty>(&target->payload))
            {
                for (std::size_t i = 0; i < set->objects.size(); ++i)
                {
                    index.emplace(keyString(set->entries[i].key), set->objects[i]);
                }
            }
        }
    }

    Document& doc_;
    const cfb::Container& c_;
    std::deque<ObjectId> pending_;
    std::set<cfb::EntryId> claimed_;
    std::map<ObjectId, std::set<cfb::EntryId>> referenced_;
};

auto Object::find(std::uint16_t pid) const -> const Property*
{
    const auto it = std::ranges::find(properties, pid, &Property::pid);
    return it == properties.end() ? nullptr : &*it;
}

auto to_string(Diagnostic::Severity severity) noexcept -> std::string_view
{
    switch (severity)
    {
        case Diagnostic::Severity::info:
            return "info";
        case Diagnostic::Severity::warning:
            return "warning";
        case Diagnostic::Severity::error:
            return "error";
    }
    return "unknown";
}

auto Document::open(const std::filesystem::path& path) -> Result<Document>
{
    auto container = cfb::Container::openFile(path);
    if (!container)
    {
        return std::unexpected(container.error());
    }
    return load(std::move(*container));
}

auto Document::load(cfb::Container container) -> Result<Document>
{
    Document doc;
    doc.container_ = std::make_unique<cfb::Container>(std::move(container));
    Loader loader(doc);
    if (auto r = loader.run(); !r)
    {
        return std::unexpected(r.error());
    }
    return doc;
}

auto Document::header() const -> ObjectId
{
    const auto* p = objects_.front().find(ids::kPidRootHeader);
    const auto* s = p == nullptr ? nullptr : std::get_if<StrongRefProperty>(&p->payload);
    return s == nullptr ? kNoObject : s->object;
}

auto Document::metaDictionary() const -> ObjectId
{
    const auto* p = objects_.front().find(ids::kPidRootMetaDictionary);
    const auto* s = p == nullptr ? nullptr : std::get_if<StrongRefProperty>(&p->payload);
    return s == nullptr ? kNoObject : s->object;
}

auto Document::classOf(ObjectId id) const -> const ClassDef*
{
    return model_.findClass(object(id).classId);
}

auto Document::propertyDef(const Property& property) const -> const PropertyDef*
{
    return model_.findPropertyByPid(property.pid);
}

auto Document::decode(const Object& object, const Property& property) const -> Result<Value>
{
    const auto* data = std::get_if<DataProperty>(&property.payload);
    if (data == nullptr)
    {
        return fail(Errc::invalid_argument, "property is not stored as data");
    }
    const auto* def = propertyDef(property);
    if (def == nullptr)
    {
        return fail(Errc::not_found, std::format("no definition for property {:#06x}", property.pid));
    }
    return decodeValue(model_, def->type, data->bytes, object.bigEndian());
}

auto Document::resolveWeak(std::uint16_t tag, std::span<const std::byte> key) const -> std::optional<ObjectId>
{
    const auto it = weakIndex_.find(tag);
    if (it == weakIndex_.end())
    {
        return std::nullopt;
    }
    const auto found = it->second.find(keyString(key));
    return found == it->second.end() ? std::nullopt : std::optional(found->second);
}

auto Document::value(ObjectId id, std::string_view className, std::string_view propertyName) const -> std::optional<Value>
{
    const auto* def = model_.findProperty(className, propertyName);
    if (def == nullptr)
    {
        return std::nullopt;
    }
    const auto& o = object(id);
    const auto* p = o.find(def->pid);
    if (p == nullptr)
    {
        return std::nullopt;
    }
    auto v = decode(o, *p);
    return v ? std::optional(std::move(*v)) : std::nullopt;
}

auto formatKey(std::span<const std::byte> key, bool bigEndian) -> std::string
{
    if (key.size() == 16)
    {
        return Auid::fromStored(key.first<16>(), bigEndian).toString();
    }
    if (key.size() == 32)
    {
        return MobId::fromStored(key.first<32>(), bigEndian).toString();
    }
    std::string out;
    for (const auto b : key)
    {
        out += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return out;
}

}
