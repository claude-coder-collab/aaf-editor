#include "fixtures.hpp"

#include <aaf/core/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

using namespace aaf;
using namespace aaf::test;

namespace
{

auto countErrors(const std::vector<Diagnostic>& diagnostics) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count(diagnostics, Diagnostic::Severity::error, &Diagnostic::severity));
}

void checkDefinitionsMatchBaseline(const Document& doc, const std::filesystem::path& path)
{
    const auto& baseline = MetaModel::baseline();
    for (const auto& [id, p] : doc.model().properties())
    {
        const auto* b = baseline.findProperty(id);
        if (b == nullptr)
        {
            continue;
        }
        INFO(path.string() << ": " << p.name);
        if (b->pid != 0)
        {
            CHECK(p.pid == b->pid);
        }
    }
}

}

TEST_CASE("AAF SDK reference files load, validate and match the baseline", "[core][document][fixtures]")
{
    for (const auto& path : aafFilesIn("aafsdk"))
    {
        INFO(path.string());
        auto doc = Document::open(path);
        REQUIRE(doc);
        CHECK(doc->header() != kNoObject);
        CHECK(doc->metaDictionary() != kNoObject);
        CHECK(doc->classOf(doc->header())->name == "Header");
        CHECK_FALSE(doc->referencedProperties().empty());
        const auto diagnostics = validate(*doc);
        for (const auto& d : diagnostics)
        {
            if (d.severity == Diagnostic::Severity::error)
            {
                INFO(d.message);
                CHECK(false);
            }
        }
        checkDefinitionsMatchBaseline(*doc, path);
    }
}

TEST_CASE("Objects, values and weak references are exposed", "[core][document]")
{
    const auto opened = Document::open(fixturesDir() / "aafsdk/test/com/OpenExistingModify/AAFHeaderTest_v102.aaf");
    REQUIRE(opened);
    const auto& doc = *opened;
    const auto header = doc.header();
    CHECK(doc.value(header, "Header", "ByteOrder")->as<std::int64_t>() == 0x4949);

    const auto extension = std::ranges::find_if(doc.model().properties(), [](const auto& e) { return e.second.name == "Header optional property"; });
    REQUIRE(extension != doc.model().properties().end());
    CHECK(doc.model().sourceOf(extension->first) == DefinitionSource::file);
    const auto* p = doc.object(header).find(extension->second.pid);
    REQUIRE(p != nullptr);
    CHECK(doc.decode(doc.object(header), *p)->toString() == "42");

    const auto content = std::get<StrongRefProperty>(doc.object(header).find(doc.model().findProperty("Header", "Content")->pid)->payload).object;
    const auto& mobs = std::get<StrongRefSetProperty>(doc.object(content).find(doc.model().findProperty("ContentStorage", "Mobs")->pid)->payload);
    REQUIRE(mobs.objects.size() == 5);
    for (std::size_t i = 0; i < mobs.objects.size(); ++i)
    {
        const auto mobId = doc.value(mobs.objects[i], "Mob", "MobID");
        REQUIRE(mobId);
        CHECK(formatKey(mobs.entries[i].key) == mobId->toString());
        CHECK(doc.value(mobs.objects[i], "Mob", "Name")->as<std::string>().starts_with("HeaderTest File Mob"));
    }

    CHECK_FALSE(doc.resolveWeak(9999, {}));
}

TEST_CASE("Weak references resolve to their targets", "[core][document]")
{
    const auto opened = Document::open(fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf");
    REQUIRE(opened);
    const auto& doc = *opened;
    const auto dataDefinitionPid = doc.model().findProperty("Component", "DataDefinition")->pid;
    std::size_t resolved = 0;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        const auto& o = doc.object(i);
        const auto* cls = doc.classOf(i);
        if (cls == nullptr || !doc.model().isA(cls->id, doc.model().findClassByName("Component")->id))
        {
            continue;
        }
        const auto* ref = o.find(dataDefinitionPid);
        REQUIRE(ref != nullptr);
        const auto& weak = std::get<WeakRefProperty>(ref->payload);
        const auto target = doc.resolveWeak(weak.tag, weak.key);
        REQUIRE(target);
        CHECK(doc.classOf(*target)->name == "DataDefinition");
        ++resolved;
    }
    CHECK(resolved > 100);
}

TEST_CASE("External reference files load and validate", "[core][document][fixtures][external]")
{
    const auto files = aafFilesIn("external");
    if (files.empty())
    {
        SKIP("external fixtures not fetched; run tools/fetch_fixtures.py");
    }
    for (const auto& path : files)
    {
        INFO(path.string());
        auto doc = Document::open(path);
        REQUIRE(doc);
        CHECK(countErrors(validate(*doc)) == 0);
        checkDefinitionsMatchBaseline(*doc, path);
    }
}

TEST_CASE("Corrupted AAF files fail cleanly", "[core][document][corruption]")
{
    const auto files = aafFilesIn("aafsdk");
    REQUIRE_FALSE(files.empty());
    const auto original = cfb::readFile(files.front()).value();
    auto container = openMemory(original);
    REQUIRE(container);

    auto builder = cfb::Builder::fromContainer(*container);
    REQUIRE(builder);
    const auto root = cfb::Builder::root();
    for (const auto child : builder->node(root).children)
    {
        auto& node = builder->node(child);
        if (node.name == u"properties")
        {
            node.data = std::vector<std::byte>{ std::byte{ 0x4C }, std::byte{ 0 }, std::byte{ 50 }, std::byte{ 0 } };
        }
    }
    auto broken = openMemory(writeToMemory(*builder).value());
    REQUIRE(broken);
    CHECK_FALSE(Document::load(std::move(*broken)));

    std::uint32_t state = 99;
    auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return state;
    };
    for (int i = 0; i < 200; ++i)
    {
        auto data = original;
        for (int k = 0; k < 8; ++k)
        {
            data[next() % data.size()] = static_cast<std::byte>(next());
        }
        if (auto c = openMemory(std::move(data)))
        {
            if (auto doc = Document::load(std::move(*c)))
            {
                (void) validate(*doc);
            }
        }
    }
    SUCCEED();
}

TEST_CASE("Weak reference indexes with zero-size keys are rejected", "[core][document][corruption]")
{
    const auto path = fixturesDir() / "aafsdk/test/com/OpenExistingModify/AAFHeaderTest_v102.aaf";
    const auto opened = Document::open(path);
    REQUIRE(opened);
    const auto& doc = *opened;

    std::vector<std::u16string> storagePath;
    std::u16string indexName;
    for (std::size_t i = 0; i < doc.objectCount() && indexName.empty(); ++i)
    {
        for (const auto& p : doc.object(i).properties)
        {
            if (const auto* weak = std::get_if<WeakRefCollectionProperty>(&p.payload))
            {
                indexName = weak->name + u" index";
                for (auto id = static_cast<ObjectId>(i); id != Document::root(); id = doc.object(id).parent)
                {
                    storagePath.insert(storagePath.begin(), doc.object(id).storageName);
                }
                break;
            }
        }
    }
    REQUIRE_FALSE(indexName.empty());

    auto container = cfb::Container::openFile(path);
    REQUIRE(container);
    auto builder = cfb::Builder::fromContainer(*container);
    REQUIRE(builder);
    auto findChild = [&](cfb::NodeId parent, std::u16string_view name) {
        for (const auto child : builder->node(parent).children)
        {
            if (builder->node(child).name == name)
            {
                return child;
            }
        }
        FAIL("missing node");
        return parent;
    };
    cfb::NodeId node = cfb::Builder::root();
    for (const auto& name : storagePath)
    {
        node = findChild(node, name);
    }
    builder->node(findChild(node, indexName)).data = std::vector<std::byte>{
        std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 5 }, std::byte{ 0 }, std::byte{ 0 }
    };
    auto broken = openMemory(writeToMemory(*builder).value());
    REQUIRE(broken);
    const auto result = Document::load(std::move(*broken));
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("malformed weak reference index") != std::string::npos);
}

namespace
{

struct EncodeStats
{
    std::size_t checked = 0;
    std::map<std::string, std::size_t> mismatches;
};

void checkEncodeInverse(const Document& doc, EncodeStats& stats)
{
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        const auto& o = doc.object(i);
        for (const auto& p : o.properties)
        {
            const auto* data = std::get_if<DataProperty>(&p.payload);
            const auto* def = doc.propertyDef(p);
            if (data == nullptr || def == nullptr)
            {
                continue;
            }
            auto decoded = decodeValue(doc.model(), def->type, data->bytes, o.bigEndian());
            if (!decoded)
            {
                continue;
            }
            ++stats.checked;
            auto encoded = encodeValue(doc.model(), def->type, *decoded, o.bigEndian());
            if (!encoded)
            {
                ++stats.mismatches[def->name + ": " + encoded.error().message];
                continue;
            }
            if (*encoded == data->bytes)
            {
                continue;
            }
            const bool paddedString = decoded->is<std::string>() && data->bytes.size() > encoded->size() && std::equal(encoded->begin(), encoded->end(), data->bytes.begin())
                && std::all_of(data->bytes.begin() + static_cast<std::ptrdiff_t>(encoded->size()), data->bytes.end(), [](std::byte b) { return b == std::byte{ 0 }; });
            auto again = decodeValue(doc.model(), def->type, *encoded, o.bigEndian());
            if (!paddedString || !again || *again != *decoded)
            {
                ++stats.mismatches[def->name];
            }
        }
    }
}

}

TEST_CASE("Encoding inverts decoding for every stored value", "[core][value][fixtures]")
{
    EncodeStats stats;
    auto files = aafFilesIn("aafsdk");
    const auto external = aafFilesIn("external");
    files.insert(files.end(), external.begin(), external.end());
    for (const auto& path : files)
    {
        auto doc = Document::open(path);
        REQUIRE(doc);
        checkEncodeInverse(*doc, stats);
    }
    INFO("checked " << stats.checked << " values");
    for (const auto& [name, count] : stats.mismatches)
    {
        UNSCOPED_INFO(name << ": " << count);
    }
    CHECK(stats.mismatches.empty());
    CHECK(stats.checked > 1000);
}

TEST_CASE("Properties and classes renamed by a file are still found by their baseline names", "[core][metamodel]")
{
    const auto opened = Document::open(fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf");
    REQUIRE(opened);
    const auto& model = opened->model();
    const auto* byFileName = model.findProperty("OperationGroup", "OperationDefinition");
    const auto* byBaselineName = model.findProperty("OperationGroup", "Operation");
    REQUIRE(byFileName != nullptr);
    CHECK(byBaselineName == byFileName);
    CHECK(byFileName->name == "OperationDefinition");
    CHECK(model.findProperty("OperationGroup", "NoSuchProperty") == nullptr);
    CHECK(model.findProperty("Mob", "Operation") == nullptr);
    CHECK(MetaModel::baseline().findProperty("OperationGroup", "OperationDefinition") == nullptr);
}
