#include "fixtures.hpp"

#include <aaf/edit/operations.hpp>
#include <aaf/edit/references.hpp>
#include <aaf/edit/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace aaf;
using namespace aaf::edit;
using namespace aaf::test;

namespace
{

auto openSample() -> Session
{
    auto s = Session::open(fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf");
    REQUIRE(s);
    return std::move(*s);
}

auto allOf(const Document& doc, std::string_view name) -> std::vector<ObjectId>
{
    const auto* cls = doc.model().findClassByName(name);
    REQUIRE(cls != nullptr);
    std::vector<ObjectId> out;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        if (doc.isAttached(i) && doc.model().isA(doc.object(i).classId, cls->id))
        {
            out.push_back(i);
        }
    }
    return out;
}

auto pidOf(const Document& doc, std::string_view cls, std::string_view property) -> std::uint16_t
{
    const auto* def = doc.model().findProperty(cls, property);
    REQUIRE(def != nullptr);
    return def->pid;
}

auto mobIdOf(const Document& doc, ObjectId id) -> MobId
{
    const auto v = doc.value(id, "Mob", "MobID");
    REQUIRE(v);
    return v->as<MobId>();
}

/// The first source clip whose SourceID resolves to a mob in the document.
auto resolvedClip(const Document& doc, const ReferenceIndex& index) -> std::pair<ObjectId, ObjectId>
{
    const auto sourceId = pidOf(doc, "SourceReference", "SourceID");
    for (const auto clip : allOf(doc, "SourceClip"))
    {
        if (const auto r = index.resolve(clip, sourceId); r && r->status == ReferenceStatus::resolved)
        {
            return { clip, r->target };
        }
    }
    FAIL("no source clip with a resolved SourceID");
    return {};
}

}

TEST_CASE("SourceIDs resolve to the mob with that MobID", "[edit][references]")
{
    auto session = openSample();
    const auto& doc = session.document();
    const ReferenceIndex index(doc);
    const auto sourceId = pidOf(doc, "SourceReference", "SourceID");
    std::size_t resolved = 0;
    std::size_t null = 0;
    for (const auto clip : allOf(doc, "SourceClip"))
    {
        const auto r = index.resolve(clip, sourceId);
        REQUIRE(r);
        const auto value = doc.value(clip, "SourceReference", "SourceID")->as<MobId>();
        if (r->status == ReferenceStatus::resolved)
        {
            ++resolved;
            CHECK(mobIdOf(doc, r->target) == value);
            CHECK(std::ranges::contains(index.referrers(r->target), Referrer{ clip, sourceId, false }));
        }
        else if (r->status == ReferenceStatus::null)
        {
            ++null;
            CHECK(value == MobId{});
        }
    }
    CHECK(resolved > 0);
    CHECK(null > 0);
}

TEST_CASE("SourceMobSlotIDs resolve to a slot of the referenced mob", "[edit][references]")
{
    auto session = openSample();
    const auto& doc = session.document();
    const ReferenceIndex index(doc);
    const auto [clip, mob] = resolvedClip(doc, index);
    const auto slotPid = pidOf(doc, "SourceReference", "SourceMobSlotID");
    const auto r = index.resolve(clip, slotPid);
    REQUIRE(r);
    REQUIRE(r->status == ReferenceStatus::resolved);
    CHECK(doc.object(r->target).parent == mob);
    CHECK(doc.value(r->target, "MobSlot", "SlotID")->toString() == doc.value(clip, "SourceReference", "SourceMobSlotID")->toString());
    const auto slots = index.candidates(clip, slotPid);
    CHECK(std::ranges::contains(slots, r->target));
    CHECK(std::ranges::all_of(slots, [&](ObjectId s) { return doc.object(s).parent == mob; }));
}

TEST_CASE("Parameter definitions, property types and generations resolve", "[edit][references]")
{
    auto session = openSample();
    const auto& doc = session.document();
    const ReferenceIndex index(doc);

    const auto definition = pidOf(doc, "Parameter", "Definition");
    const auto parameters = allOf(doc, "Parameter");
    REQUIRE_FALSE(parameters.empty());
    for (const auto p : parameters)
    {
        const auto r = index.resolve(p, definition);
        REQUIRE(r);
        REQUIRE(r->status == ReferenceStatus::resolved);
        CHECK(doc.model().isA(doc.object(r->target).classId, doc.model().findClassByName("ParameterDefinition")->id));
    }

    const auto type = pidOf(doc, "PropertyDefinition", "Type");
    for (const auto p : allOf(doc, "PropertyDefinition"))
    {
        const auto r = index.resolve(p, type);
        REQUIRE(r);
        CHECK((r->status == ReferenceStatus::resolved || r->status == ReferenceStatus::builtin));
    }

    CHECK(index.definition(Document::root(), pidOf(doc, "SourceReference", "SourceID")) == nullptr);
}

TEST_CASE("Implicit references can be retargeted", "[edit][references]")
{
    auto session = openSample();
    const auto& doc = session.document();
    const auto sourceId = pidOf(doc, "SourceReference", "SourceID");
    const auto [clip, mob] = [&] {
        const ReferenceIndex index(doc);
        return resolvedClip(doc, index);
    }();
    const ReferenceIndex index(doc);
    const auto mobs = index.candidates(clip, sourceId);
    REQUIRE(mobs.size() > 1);
    const auto other = *std::ranges::find_if(mobs, [&](ObjectId m) { return m != mob; });
    CHECK(index.valueFor(clip, sourceId, other)->as<MobId>() == mobIdOf(doc, other));
    CHECK_FALSE(index.valueFor(clip, sourceId, clip));

    REQUIRE(session.execute("retarget", [&](Transaction& tx) { return setReference(tx, clip, sourceId, other); }));
    CHECK(ReferenceIndex(doc).resolve(clip, sourceId)->target == other);
    REQUIRE(session.undo());
    CHECK(ReferenceIndex(doc).resolve(clip, sourceId)->target == mob);
}

TEST_CASE("Changing an identifier can rewrite the references to it", "[edit][references]")
{
    auto session = openSample();
    const auto& doc = session.document();
    const auto mobIdPid = pidOf(doc, "Mob", "MobID");
    const auto [clip, mob] = [&] {
        const ReferenceIndex index(doc);
        return resolvedClip(doc, index);
    }();
    const auto before = ReferenceIndex(doc).referrersByKey(mob, mobIdPid);
    REQUIRE_FALSE(before.empty());

    const auto renamed = MobId::generate();
    REQUIRE(session.execute("rename", [&](Transaction& tx) { return setIdentifier(tx, mob, mobIdPid, Value(renamed)); }));
    {
        const ReferenceIndex index(doc);
        CHECK(index.referrersByKey(mob, mobIdPid).size() == before.size());
        for (const auto& r : before)
        {
            CHECK(index.resolve(r.object, r.pid)->target == mob);
        }
    }
    REQUIRE(session.undo());

    REQUIRE(session.execute("rename only", [&](Transaction& tx) { return setProperty(tx, mob, mobIdPid, Value(MobId::generate())); }));
    const ReferenceIndex index(doc);
    CHECK(index.referrersByKey(mob, mobIdPid).empty());
    CHECK(index.resolve(clip, pidOf(doc, "SourceReference", "SourceID"))->status == ReferenceStatus::external);
}

TEST_CASE("References from inside a subtree do not count as incoming", "[edit][references]")
{
    auto session = openSample();
    const auto& doc = session.document();
    const ReferenceIndex index(doc);
    const auto [clip, mob] = resolvedClip(doc, index);
    const auto incoming = incomingImplicitReferences(doc, index, mob);
    CHECK(std::ranges::contains(incoming, Referrer{ clip, pidOf(doc, "SourceReference", "SourceID"), false }));
    CHECK(incomingImplicitReferences(doc, index, clip).empty());
    CHECK(incomingImplicitReferences(doc, Document::root()).empty());
}
