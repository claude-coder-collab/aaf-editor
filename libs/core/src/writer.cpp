#include <aaf/core/writer.hpp>

#include <algorithm>
#include <format>

namespace aaf
{

namespace
{

using namespace literals;

constexpr Auid kSignature512 = "42464141-000d-4d4f-060e-2b34010101ff"_auid;
constexpr Auid kSignature4K = "0d010201-0200-0000-060e-2b3403020101"_auid;
constexpr std::size_t kMaxElementSuffix = 10;

class Encoder
{
public:
    explicit Encoder(bool bigEndian) :
        bigEndian_(bigEndian)
    {
    }

    template <std::unsigned_integral T>
    void put(T value)
    {
        for (std::size_t i = 0; i < sizeof(T); ++i)
        {
            const std::size_t shift = (bigEndian_ ? sizeof(T) - 1 - i : i) * 8;
            out_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }
    void put(std::span<const std::byte> bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }
    void putName(std::u16string_view name)
    {
        for (const auto c : name)
        {
            put(static_cast<std::uint16_t>(c));
        }
        put(std::uint16_t{ 0 });
    }
    [[nodiscard]] auto take() -> std::vector<std::byte> { return std::move(out_); }

private:
    bool bigEndian_;
    std::vector<std::byte> out_;
};

auto elementName(std::u16string_view base, std::uint32_t key) -> std::u16string
{
    std::u16string name(base);
    name += u'{';
    for (const char c : std::format("{:x}", key))
    {
        name.push_back(static_cast<char16_t>(c));
    }
    name += u'}';
    return name;
}

auto toClsid(const Auid& id) -> cfb::Clsid
{
    cfb::Clsid clsid;
    clsid.bytes = id.bytes;
    return clsid;
}

class Writer
{
public:
    Writer(const Document& doc, const WriteOptions& options) :
        doc_(doc),
        src_(doc.container()),
        options_(options),
        builder_(options.version.value_or(doc.container().header().version))
    {
    }

    auto run() -> Result<cfb::Builder>
    {
        const auto version = builder_.version();
        auto headerClsid = src_.header().clsid;
        const Auid signature{ headerClsid.bytes };
        if (signature == kSignature512 || signature == kSignature4K || signature.isNull())
        {
            headerClsid = toClsid(version == cfb::Version::v3 ? kSignature512 : kSignature4K);
        }
        builder_.setHeaderClsid(headerClsid);
        copyMetadata(src_.root(), builder_.node(cfb::Builder::root()));
        if (auto r = writeObject(Document::root(), cfb::Builder::root()); !r)
        {
            return std::unexpected(r.error());
        }
        if (!doc_.referencedProperties().empty())
        {
            if (auto r = addStream(cfb::Builder::root(), u"referenced properties", referencedProperties(), 0); !r)
            {
                return std::unexpected(r.error());
            }
        }
        return std::move(builder_);
    }

private:
    static void copyMetadata(const cfb::DirEntry& entry, cfb::BuildNode& node)
    {
        node.clsid = entry.clsid;
        node.stateBits = entry.stateBits;
        node.creationTime = entry.creationTime;
        node.modifiedTime = entry.modifiedTime;
    }

    [[nodiscard]] auto referencedProperties() const -> std::vector<std::byte>
    {
        const auto& paths = doc_.referencedProperties();
        const auto byteOrder = doc_.referencedPropertiesByteOrder();
        Encoder e(byteOrder == 0x42);
        e.put(byteOrder);
        std::uint32_t pidCount = 0;
        for (const auto& path : paths)
        {
            pidCount += static_cast<std::uint32_t>(path.size() + 1);
        }
        e.put(static_cast<std::uint16_t>(paths.size()));
        e.put(pidCount);
        for (const auto& path : paths)
        {
            for (const auto pid : path)
            {
                e.put(pid);
            }
            e.put(std::uint16_t{ 0 });
        }
        return e.take();
    }

    [[nodiscard]] auto storageName(const Property& p, std::u16string_view original) const -> std::u16string
    {
        if (options_.preserveLayout)
        {
            return std::u16string(original);
        }
        const auto* def = doc_.propertyDef(p);
        return generatedStorageName(def != nullptr ? def->name : "Property", p.pid);
    }

    auto addStorage(cfb::NodeId parent, const std::u16string& name, ObjectId child) -> Result<void>
    {
        auto node = builder_.addStorage(parent, name);
        if (!node)
        {
            return std::unexpected(node.error());
        }
        return writeObject(child, *node);
    }

    auto addStream(cfb::NodeId parent, const std::u16string& name, cfb::StreamData data, cfb::EntryId sourceStorage) -> Result<void>
    {
        auto node = builder_.addStream(parent, name, std::move(data));
        if (!node)
        {
            return std::unexpected(node.error());
        }
        if (sourceStorage != cfb::kNoStream)
        {
            if (const auto entry = src_.find(sourceStorage, name))
            {
                copyMetadata(src_.entry(*entry), builder_.node(*node));
            }
        }
        return {};
    }

    auto writeObject(ObjectId id, cfb::NodeId node) -> Result<void>
    {
        const auto& o = doc_.object(id);
        if (o.storage != cfb::kNoStream)
        {
            copyMetadata(src_.entry(o.storage), builder_.node(node));
        }
        builder_.node(node).clsid = toClsid(o.classId);

        const bool be = o.bigEndian();
        std::vector<std::vector<std::byte>> values;
        values.reserve(o.properties.size());
        for (const auto& p : o.properties)
        {
            auto value = writeProperty(o, p, node);
            if (!value)
            {
                return std::unexpected(value.error());
            }
            if (value->size() > 0xFFFF)
            {
                return fail(Errc::limit, std::format("property {:#06x} value exceeds 65535 bytes", p.pid));
            }
            values.push_back(std::move(*value));
        }

        Encoder e(be);
        e.put(o.byteOrder);
        e.put(o.formatVersion);
        e.put(static_cast<std::uint16_t>(o.properties.size()));
        for (std::size_t i = 0; i < o.properties.size(); ++i)
        {
            e.put(o.properties[i].pid);
            e.put(o.properties[i].storedForm);
            e.put(static_cast<std::uint16_t>(values[i].size()));
        }
        for (const auto& v : values)
        {
            e.put(v);
        }
        if (auto r = addStream(node, u"properties", e.take(), o.storage); !r)
        {
            return r;
        }
        for (const auto entry : o.extraEntries)
        {
            if (auto r = copyEntry(entry, node); !r)
            {
                return r;
            }
        }
        return {};
    }

    auto writeProperty(const Object& o, const Property& p, cfb::NodeId node) -> Result<std::vector<std::byte>>
    {
        const bool be = o.bigEndian();
        Encoder e(be);
        const auto result = std::visit(
            [&](const auto& payload) -> Result<void> {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, DataProperty> || std::is_same_v<T, UnknownProperty>)
                {
                    e.put(payload.bytes);
                    return {};
                }
                else if constexpr (std::is_same_v<T, StrongRefProperty>)
                {
                    const auto name = storageName(p, payload.name);
                    e.putName(name);
                    return addStorage(node, name, payload.object);
                }
                else if constexpr (std::is_same_v<T, StrongRefVectorProperty>)
                {
                    const auto name = storageName(p, payload.name);
                    e.putName(name);
                    return writeVector(payload, name, node, be, o.storage);
                }
                else if constexpr (std::is_same_v<T, StrongRefSetProperty>)
                {
                    const auto name = storageName(p, payload.name);
                    e.putName(name);
                    return writeSet(payload, name, node, be, o.storage);
                }
                else if constexpr (std::is_same_v<T, WeakRefProperty>)
                {
                    e.put(payload.tag);
                    e.put(payload.keyPid);
                    e.put(static_cast<std::uint8_t>(payload.key.size()));
                    e.put(payload.key);
                    return {};
                }
                else if constexpr (std::is_same_v<T, WeakRefCollectionProperty>)
                {
                    const auto name = storageName(p, payload.name);
                    e.putName(name);
                    Encoder index(be);
                    index.put(static_cast<std::uint32_t>(payload.keys.size()));
                    index.put(payload.tag);
                    index.put(payload.keyPid);
                    index.put(payload.keySize);
                    for (const auto& key : payload.keys)
                    {
                        index.put(key);
                    }
                    return addStream(node, name + u" index", index.take(), options_.preserveLayout ? o.storage : cfb::kNoStream);
                }
                else
                {
                    static_assert(std::is_same_v<T, StreamProperty>);
                    const auto name = storageName(p, payload.name);
                    e.put(payload.byteOrder);
                    e.putName(name);
                    if (payload.data)
                    {
                        return addStream(node, name, *payload.data, options_.preserveLayout ? o.storage : cfb::kNoStream);
                    }
                    if (payload.entry == cfb::kNoStream)
                    {
                        return {};
                    }
                    return addStream(node, name, cfb::SourceStream{ &src_, payload.entry }, options_.preserveLayout ? o.storage : cfb::kNoStream);
                }
            },
            p.payload
        );
        if (!result)
        {
            return std::unexpected(result.error());
        }
        return e.take();
    }

    auto writeVector(const StrongRefVectorProperty& v, const std::u16string& name, cfb::NodeId node, bool be, cfb::EntryId sourceStorage) -> Result<void>
    {
        const auto count = static_cast<std::uint32_t>(v.objects.size());
        Encoder index(be);
        index.put(count);
        index.put(options_.preserveLayout ? v.firstFreeKey : count);
        index.put(options_.preserveLayout ? v.lastFreeKey : ~std::uint32_t{ 0 });
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto key = options_.preserveLayout ? v.localKeys[i] : i;
            index.put(key);
            if (auto r = addStorage(node, elementName(name, key), v.objects[i]); !r)
            {
                return r;
            }
        }
        return addStream(node, name + u" index", index.take(), options_.preserveLayout ? sourceStorage : cfb::kNoStream);
    }

    auto writeSet(const StrongRefSetProperty& s, const std::u16string& name, cfb::NodeId node, bool be, cfb::EntryId sourceStorage) -> Result<void>
    {
        const auto count = static_cast<std::uint32_t>(s.objects.size());
        Encoder index(be);
        index.put(count);
        index.put(options_.preserveLayout ? s.firstFreeKey : count);
        index.put(options_.preserveLayout ? s.lastFreeKey : ~std::uint32_t{ 0 });
        index.put(s.keyPid);
        index.put(s.keySize);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& entry = s.entries[i];
            const auto key = options_.preserveLayout ? entry.localKey : i;
            index.put(key);
            index.put(entry.referenceCount);
            index.put(entry.key);
            if (auto r = addStorage(node, elementName(name, key), s.objects[i]); !r)
            {
                return r;
            }
        }
        return addStream(node, name + u" index", index.take(), options_.preserveLayout ? sourceStorage : cfb::kNoStream);
    }

    auto copyEntry(cfb::EntryId entry, cfb::NodeId parent) -> Result<void>
    {
        const auto& e = src_.entry(entry);
        if (e.isStream())
        {
            auto node = builder_.addStream(parent, e.name, cfb::SourceStream{ &src_, entry });
            if (!node)
            {
                return std::unexpected(node.error());
            }
            copyMetadata(e, builder_.node(*node));
            return {};
        }
        auto node = builder_.addStorage(parent, e.name);
        if (!node)
        {
            return std::unexpected(node.error());
        }
        copyMetadata(e, builder_.node(*node));
        for (const auto child : e.children)
        {
            if (auto r = copyEntry(child, *node); !r)
            {
                return r;
            }
        }
        return {};
    }

    const Document& doc_;
    const cfb::Container& src_;
    const WriteOptions& options_;
    cfb::Builder builder_;
};

}

auto generatedStorageName(std::string_view propertyName, std::uint16_t pid) -> std::u16string
{
    const auto suffix = std::format("-{:x}", pid);
    const std::size_t maxBase = cfb::kMaxNameLength - kMaxElementSuffix - suffix.size();
    std::u16string name;
    for (const char c : propertyName)
    {
        if (name.size() >= maxBase)
        {
            break;
        }
        const bool legal = c != '/' && c != '\\' && c != ':' && c != '!' && c != '{' && c != '}' && static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x80;
        name.push_back(legal ? static_cast<char16_t>(c) : u'_');
    }
    for (const char c : suffix)
    {
        name.push_back(static_cast<char16_t>(c));
    }
    return name;
}

auto buildContainer(const Document& document, const WriteOptions& options) -> Result<cfb::Builder>
{
    return Writer(document, options).run();
}

auto write(const Document& document, cfb::ByteSink& sink, const WriteOptions& options) -> Result<void>
{
    auto builder = buildContainer(document, options);
    if (!builder)
    {
        return std::unexpected(builder.error());
    }
    return cfb::write(*builder, sink);
}

auto save(const Document& document, const std::filesystem::path& path, const WriteOptions& options) -> Result<void>
{
    auto builder = buildContainer(document, options);
    if (!builder)
    {
        return std::unexpected(builder.error());
    }
    return cfb::writeFile(*builder, path);
}

}
