#include "fixtures.hpp"

#include <aaf/core/document.hpp>
#include <aaf/core/writer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <format>
#include <random>

using namespace aaf;
using namespace aaf::test;

namespace
{

auto roundTrip(const Document& doc, const WriteOptions& options) -> Result<Document>
{
    cfb::MemorySink sink;
    if (auto r = write(doc, sink, options); !r)
    {
        return std::unexpected(r.error());
    }
    auto container = openMemory(sink.take());
    if (!container)
    {
        return std::unexpected(container.error());
    }
    return Document::load(std::move(*container));
}

auto streamBytes(const Document& doc, cfb::EntryId entry) -> std::vector<std::byte>
{
    if (entry == cfb::kNoStream)
    {
        return {};
    }
    return doc.container().openStream(entry).value().readAll().value();
}

/// Compares two object trees ignoring storage names and local keys.
auto semanticDiff(const Document& a, ObjectId ia, const Document& b, ObjectId ib) -> std::string
{
    const auto& oa = a.object(ia);
    const auto& ob = b.object(ib);
    if (oa.classId != ob.classId || oa.properties.size() != ob.properties.size() || oa.extraEntries.size() != ob.extraEntries.size())
    {
        return std::format("object {}: class or property count differs", ia);
    }
    for (std::size_t i = 0; i < oa.properties.size(); ++i)
    {
        const auto& pa = oa.properties[i];
        const auto& pb = ob.properties[i];
        if (pa.pid != pb.pid || pa.storedForm != pb.storedForm || pa.payload.index() != pb.payload.index())
        {
            return std::format("object {} property {:#06x}: header differs", ia, pa.pid);
        }
        auto children = [&](const std::vector<ObjectId>& ca, const std::vector<ObjectId>& cb) -> std::string {
            if (ca.size() != cb.size())
            {
                return std::format("object {} property {:#06x}: element count differs", ia, pa.pid);
            }
            for (std::size_t k = 0; k < ca.size(); ++k)
            {
                if (auto d = semanticDiff(a, ca[k], b, cb[k]); !d.empty())
                {
                    return d;
                }
            }
            return {};
        };
        std::string d;
        if (const auto* x = std::get_if<DataProperty>(&pa.payload))
        {
            d = x->bytes == std::get<DataProperty>(pb.payload).bytes ? "" : "data differs";
        }
        else if (const auto* s = std::get_if<StrongRefProperty>(&pa.payload))
        {
            d = semanticDiff(a, s->object, b, std::get<StrongRefProperty>(pb.payload).object);
        }
        else if (const auto* v = std::get_if<StrongRefVectorProperty>(&pa.payload))
        {
            d = children(v->objects, std::get<StrongRefVectorProperty>(pb.payload).objects);
        }
        else if (const auto* set = std::get_if<StrongRefSetProperty>(&pa.payload))
        {
            const auto& other = std::get<StrongRefSetProperty>(pb.payload);
            const bool keysEqual = std::ranges::equal(set->entries, other.entries, [](const SetEntry& l, const SetEntry& r) { return l.key == r.key && l.referenceCount == r.referenceCount; });
            d = keysEqual ? children(set->objects, other.objects) : "set keys differ";
        }
        else if (const auto* w = std::get_if<WeakRefProperty>(&pa.payload))
        {
            const auto& other = std::get<WeakRefProperty>(pb.payload);
            d = w->tag == other.tag && w->key == other.key ? "" : "weak reference differs";
        }
        else if (const auto* wc = std::get_if<WeakRefCollectionProperty>(&pa.payload))
        {
            const auto& other = std::get<WeakRefCollectionProperty>(pb.payload);
            d = wc->tag == other.tag && wc->keys == other.keys ? "" : "weak collection differs";
        }
        else if (const auto* st = std::get_if<StreamProperty>(&pa.payload))
        {
            d = streamBytes(a, st->entry) == streamBytes(b, std::get<StreamProperty>(pb.payload).entry) ? "" : "stream differs";
        }
        else
        {
            d = std::get<UnknownProperty>(pa.payload).bytes == std::get<UnknownProperty>(pb.payload).bytes ? "" : "unknown property differs";
        }
        if (!d.empty())
        {
            return d.starts_with("object") ? d : std::format("object {} property {:#06x}: {}", ia, pa.pid, d);
        }
    }
    return {};
}

void checkLossless(const std::filesystem::path& path)
{
    INFO(path.string());
    auto original = Document::open(path);
    REQUIRE(original);
    for (const auto version : { cfb::Version::v3, cfb::Version::v4 })
    {
        auto copy = roundTrip(*original, { .preserveLayout = true, .version = version });
        REQUIRE(copy);
        CHECK(diffTrees(original->container(), copy->container()) == "");
    }
    auto regenerated = roundTrip(*original, { .preserveLayout = false, .version = std::nullopt });
    REQUIRE(regenerated);
    CHECK(semanticDiff(*original, Document::root(), *regenerated, Document::root()) == "");
    CHECK(std::ranges::count(validate(*regenerated), Diagnostic::Severity::error, &Diagnostic::severity) == 0);
}

auto tempDir() -> std::filesystem::path
{
    auto dir = std::filesystem::temp_directory_path() / std::format("aaf-writer-{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    return dir;
}

}

TEST_CASE("AAF SDK reference files round-trip losslessly", "[core][writer][fixtures]")
{
    for (const auto& path : aafFilesIn("aafsdk"))
    {
        checkLossless(path);
    }
}

TEST_CASE("External reference files round-trip losslessly", "[core][writer][fixtures][external]")
{
    const auto files = aafFilesIn("external");
    if (files.empty())
    {
        SKIP("external fixtures not fetched; run tools/fetch_fixtures.py");
    }
    for (const auto& path : files)
    {
        checkLossless(path);
    }
}

TEST_CASE("Unknown properties and extra storage entries survive a save", "[core][writer]")
{
    const auto path = aafFilesIn("aafsdk").front();
    auto container = cfb::Container::openFile(path);
    REQUIRE(container);
    auto builder = cfb::Builder::fromContainer(*container);
    REQUIRE(builder);

    cfb::NodeId header = 0;
    for (const auto child : builder->node(cfb::Builder::root()).children)
    {
        if (builder->node(child).name == u"Header-2")
        {
            header = child;
        }
    }
    REQUIRE(header != 0);
    for (const auto child : builder->node(header).children)
    {
        auto& node = builder->node(child);
        if (node.name != u"properties")
        {
            continue;
        }
        const auto& source = std::get<cfb::SourceStream>(node.data);
        auto bytes = source.container->openStream(source.id).value().readAll().value();
        const auto count = static_cast<std::size_t>(std::to_integer<unsigned>(bytes[2]) | (std::to_integer<unsigned>(bytes[3]) << 8U));
        const std::array<std::byte, 6> entry = { std::byte{ 0x34 }, std::byte{ 0x12 }, std::byte{ 0x86 }, std::byte{ 0x00 }, std::byte{ 3 }, std::byte{ 0 } };
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(4 + count * 6), entry.begin(), entry.end());
        bytes.insert(bytes.end(), { std::byte{ 0xAA }, std::byte{ 0xBB }, std::byte{ 0xCC } });
        bytes[2] = static_cast<std::byte>((count + 1) & 0xFF);
        bytes[3] = static_cast<std::byte>((count + 1) >> 8U);
        node.data = std::move(bytes);
    }
    const auto vendor = builder->addStorage(header, u"Vendor Private").value();
    REQUIRE(builder->addStream(vendor, u"blob", bytesOf("vendor data")));
    REQUIRE(builder->addStream(cfb::Builder::root(), u"\x05SummaryInformation", bytesOf("summary")));

    auto modified = openMemory(writeToMemory(*builder).value());
    REQUIRE(modified);
    auto doc = Document::load(std::move(*modified));
    REQUIRE(doc);
    const auto& h = doc->object(doc->header());
    const auto* unknown = h.find(0x1234);
    REQUIRE(unknown != nullptr);
    CHECK(std::holds_alternative<UnknownProperty>(unknown->payload));
    CHECK(h.extraEntries.size() == 1);
    CHECK(doc->object(Document::root()).extraEntries.size() == 1);

    auto copy = roundTrip(*doc, {});
    REQUIRE(copy);
    CHECK(diffTrees(doc->container(), copy->container()) == "");
    auto regenerated = roundTrip(*doc, { .preserveLayout = false, .version = std::nullopt });
    REQUIRE(regenerated);
    CHECK(semanticDiff(*doc, Document::root(), *regenerated, Document::root()) == "");
}

TEST_CASE("A document can be saved over its own source file", "[core][writer]")
{
    const auto dir = tempDir();
    const auto path = dir / "edit.aaf";
    std::filesystem::copy_file(aafFilesIn("aafsdk").back(), path);
    {
        auto doc = Document::open(path);
        REQUIRE(doc);
        REQUIRE(save(*doc, path, { .preserveLayout = false, .version = cfb::Version::v4 }));
        REQUIRE(save(*doc, path));
        auto reopened = Document::open(path);
        REQUIRE(reopened);
        CHECK(diffTrees(doc->container(), reopened->container()) == "");
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("Generated storage names are legal and bounded", "[core][writer]")
{
    CHECK(generatedStorageName("Mobs", 0x1901) == u"Mobs-1901");
    CHECK(generatedStorageName("Header optional property", 0xffe0) == u"Header optional -ffe0");
    CHECK(generatedStorageName("a/b:c{d}", 0x10) == u"a_b_c_d_-10");
    const auto longest = generatedStorageName(std::string(100, 'x'), 0xffff);
    CHECK(longest.size() + 10 <= cfb::kMaxNameLength);
    CHECK(cfb::validateName(longest + u"{ffffffff}"));
}
