#include <aaf/core/metamodel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace aaf;
using namespace aaf::literals;

TEST_CASE("The baseline model contains the AAF object model", "[core][metamodel]")
{
    const auto& m = MetaModel::baseline();
    CHECK(m.classes().size() == 116);
    CHECK(m.types().size() == 164);

    const auto* header = m.findClassByName("Header");
    REQUIRE(header != nullptr);
    CHECK(header->id == "0d010101-0101-2f00-060e-2b3402060101"_auid);
    CHECK(header->concrete);
    CHECK(m.isA(header->id, m.findClassByName("InterchangeObject")->id));
    CHECK_FALSE(m.isA(header->id, m.findClassByName("Mob")->id));

    const auto* root = m.findClass(ids::kRootClass);
    REQUIRE(root != nullptr);
    CHECK(root->name == "Root");

    const auto* name = m.findProperty("Mob", "Name");
    REQUIRE(name != nullptr);
    CHECK(name->pid == 0x4402);
    CHECK(m.findPropertyByPid(0x4402) == name);
    CHECK(m.findProperty("Mob", "NoSuchProperty") == nullptr);
    CHECK(m.findProperty("NoSuchClass", "Name") == nullptr);
}

TEST_CASE("Inherited properties are listed root class first", "[core][metamodel]")
{
    const auto& m = MetaModel::baseline();
    const auto props = m.allProperties(m.findClassByName("SourceClip")->id);
    REQUIRE_FALSE(props.empty());
    CHECK(props.front()->name == "ObjClass");
    const auto has = [&](std::string_view n) { return std::ranges::any_of(props, [n](const PropertyDef* p) { return p->name == n; }); };
    CHECK(has("Length"));
    CHECK(has("SourceID"));
    CHECK(has("StartTime"));
    CHECK_FALSE(has("Slots"));
}

TEST_CASE("Type sizes and renames resolve", "[core][metamodel]")
{
    const auto& m = MetaModel::baseline();
    const auto position = "01012001-0000-0000-060e-2b3401040101"_auid;
    REQUIRE(m.resolve(position) != nullptr);
    CHECK(m.resolve(position)->kind == TypeKind::integer);
    CHECK(m.fixedSize(position) == 8);
    CHECK(m.fixedSize(ids::kTypeAuid) == 16);
    CHECK(m.fixedSize(ids::kTypeMobId) == 32);
    CHECK(m.fixedSize(ids::kTypeBoolean) == 1);
    CHECK(m.fixedSize("03010100-0000-0000-060e-2b3401040101"_auid) == 8);
    CHECK_FALSE(m.fixedSize(ids::kTypeString));
    CHECK_FALSE(m.fixedSize("00000000-0000-0000-0000-000000000000"_auid));
}

TEST_CASE("Definitions can be added and replaced", "[core][metamodel]")
{
    MetaModel m = MetaModel::baseline();
    const auto id = "11111111-2222-3333-4444-555555555555"_auid;
    PropertyDef p;
    p.id = id;
    p.name = "Custom";
    p.pid = 0xFFF0;
    p.type = ids::kTypeString;
    m.addProperty(p, DefinitionSource::file);
    CHECK(m.findPropertyByPid(0xFFF0)->name == "Custom");
    CHECK(m.sourceOf(id) == DefinitionSource::file);
    p.pid = 0xFFF1;
    m.addProperty(p, DefinitionSource::file);
    CHECK(m.findPropertyByPid(0xFFF0) == nullptr);
    CHECK(m.findPropertyByPid(0xFFF1)->name == "Custom");
    CHECK(MetaModel::baseline().findProperty(id) == nullptr);
}
