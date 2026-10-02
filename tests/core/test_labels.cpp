#include "fixtures.hpp"

#include <aaf/core/document.hpp>
#include <aaf/core/labels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace aaf;
using namespace aaf::literals;

TEST_CASE("SMPTE labels are found by AUID", "[core][labels]")
{
    const auto op1a = "0d010201-0101-0100-060e-2b3404010101"_auid;
    REQUIRE(isSmpteUl(op1a));
    const auto* label = findSmpteLabel(op1a);
    REQUIRE(label != nullptr);
    CHECK(label->name == "MXF OP1a SingleItem SinglePackage UniTrack Stream Internal");
    CHECK(label->id == op1a);

    const auto* picture = findSmpteLabel("01030202-0100-0000-060e-2b3404010101"_auid);
    REQUIRE(picture != nullptr);
    CHECK(picture->name == "Picture Essence Track");
}

TEST_CASE("SMPTE label lookup ignores the UL version byte", "[core][labels]")
{
    const auto* registered = findSmpteLabel("0d010201-0101-0100-060e-2b3404010101"_auid);
    const auto* otherVersion = findSmpteLabel("0d010201-0101-0100-060e-2b3404010105"_auid);
    REQUIRE(otherVersion != nullptr);
    CHECK(otherVersion == registered);
}

TEST_CASE("Identifiers that are not registered labels have no name", "[core][labels]")
{
    CHECK_FALSE(isSmpteUl(Auid{}));
    CHECK(findSmpteLabel(Auid{}) == nullptr);
    for (int i = 0; i < 20; ++i)
    {
        CHECK(findSmpteLabel(Auid::generate()) == nullptr);
    }
    CHECK(findSmpteLabel("0d010201-0101-0100-060e-2b3404010199"_auid) != nullptr);
    CHECK(findSmpteLabel("7f7f7f7f-7f7f-7f7f-060e-2b3404010101"_auid) == nullptr);
    CHECK(smpteLabelFamily(Auid::generate()).empty());
}

TEST_CASE("A label's family holds the related leaf labels", "[core][labels]")
{
    const auto op1a = "0d010201-0101-0100-060e-2b3404010101"_auid;
    const auto family = smpteLabelFamily(op1a);
    CHECK(std::ranges::any_of(family, [&](const SmpteLabel* l) { return l->id == op1a; }));
    CHECK(std::ranges::any_of(family, [](const SmpteLabel* l) { return l->name.contains("OP1b"); }));
    CHECK(std::ranges::any_of(family, [](const SmpteLabel* l) { return l->name.contains("Atom"); }));
    CHECK(std::ranges::none_of(family, [](const SmpteLabel* l) { return l->node; }));
    CHECK(std::ranges::none_of(family, [](const SmpteLabel* l) { return l->name.contains("Essence Track"); }));
}

TEST_CASE("Definitions in the sample files are registered labels", "[core][labels]")
{
    auto doc = Document::open(test::fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf");
    REQUIRE(doc);
    std::size_t named = 0;
    for (std::size_t i = 0; i < doc->objectCount(); ++i)
    {
        const auto* cls = doc->classOf(i);
        if (cls == nullptr || cls->name != "DataDefinition")
        {
            continue;
        }
        if (const auto v = doc->value(i, "DefinitionObject", "Identification"); v && v->is<Auid>() && findSmpteLabel(v->as<Auid>()) != nullptr)
        {
            ++named;
        }
    }
    CHECK(named >= 2);
}
