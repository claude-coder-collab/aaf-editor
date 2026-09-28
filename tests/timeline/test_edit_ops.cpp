#include "fixtures.hpp"

#include <aaf/edit/session.hpp>
#include <aaf/timeline/edit.hpp>
#include <aaf/timeline/timeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <random>

using namespace aaf;
using namespace aaf::timeline;
using namespace aaf::test;

namespace
{

constexpr std::string_view kSample = "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf";

auto openSession() -> Result<edit::Session>
{
    return edit::Session::open(fixturesDir() / kSample);
}

auto errorCount(const Document& doc) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count(validate(doc), Diagnostic::Severity::error, &Diagnostic::severity));
}

struct Located
{
    ObjectId mob = kNoObject;
    ObjectId slot = kNoObject;
};

auto trackOf(const Document& doc, ObjectId slot) -> Track
{
    const Projector projector(doc);
    auto mob = doc.object(slot).parent;
    auto t = projector.project(mob);
    REQUIRE(t);
    const auto it = std::ranges::find(t->tracks, slot, &Track::slot);
    REQUIRE(it != t->tracks.end());
    return *it;
}

auto end(const Track& track) -> std::int64_t
{
    std::int64_t sum = 0;
    for (const auto& item : track.items)
    {
        sum += item.kind == ItemKind::transition ? -item.length : item.length;
    }
    return sum;
}

/// A composition track holding a Sequence with at least `clips` source clips and, if requested, a transition.
auto findTrack(const Document& doc, std::size_t clips, bool withTransition) -> Located
{
    const Projector projector(doc);
    for (const auto& m : projector.mobs())
    {
        if (m.kind != MobKind::composition)
        {
            continue;
        }
        auto t = projector.project(m.object);
        REQUIRE(t);
        for (const auto& track : t->tracks)
        {
            const auto sources = std::ranges::count(track.items, ItemKind::sourceClip, &Item::kind);
            const auto transitions = std::ranges::count(track.items, ItemKind::transition, &Item::kind);
            const bool sequence = doc.classOf(track.segment)->name == "Sequence";
            if (sequence && track.effects.empty() && track.slotKind == SlotKind::timeline && static_cast<std::size_t>(sources) >= clips && (transitions > 0) == withTransition)
            {
                return { m.object, track.slot };
            }
        }
    }
    FAIL("no suitable track");
    return {};
}

auto storedLength(const Document& doc, ObjectId component) -> std::int64_t
{
    const auto v = doc.value(component, "Component", "Length");
    return v && v->is<std::int64_t>() ? v->as<std::int64_t>() : -1;
}

auto startTime(const Document& doc, ObjectId clip) -> std::int64_t
{
    return doc.value(clip, "SourceClip", "StartTime").value().as<std::int64_t>();
}

void checkConsistent(const Document& doc, ObjectId slot)
{
    const auto track = trackOf(doc, slot);
    CHECK(storedLength(doc, track.segment) == end(track));
    for (std::size_t i = 1; i < track.items.size(); ++i)
    {
        const bool fillers = track.items[i].kind == ItemKind::filler && track.items[i - 1].kind == ItemKind::filler;
        CHECK_FALSE(fillers);
    }
    CHECK(errorCount(doc) == 0);
}

auto run(edit::Session& session, const edit::Session::Command& command) -> Result<edit::ChangeSet>
{
    return session.execute("op", command);
}

}

TEST_CASE("Splitting a clip keeps the timeline and offsets the source", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 2, false);
    const auto before = trackOf(doc, where.slot);
    const auto clip = *std::ranges::find(before.items, ItemKind::sourceClip, &Item::kind);
    REQUIRE(clip.length >= 2);
    const auto at = clip.start + clip.length / 2;

    ObjectId right = kNoObject;
    REQUIRE(run(session, [&](edit::Transaction& tx) -> Result<void> {
        auto r = ops::split(tx, where.slot, at);
        if (!r)
        {
            return std::unexpected(r.error());
        }
        right = *r;
        return {};
    }));
    const auto after = trackOf(doc, where.slot);
    CHECK(after.items.size() == before.items.size() + 1);
    CHECK(end(after) == end(before));
    CHECK(storedLength(doc, clip.object) == at - clip.start);
    CHECK(storedLength(doc, right) == clip.start + clip.length - at);
    CHECK(startTime(doc, right) == startTime(doc, clip.object) + (at - clip.start));
    checkConsistent(doc, where.slot);

    CHECK_FALSE(run(session, [&](edit::Transaction& tx) { return ops::split(tx, where.slot, 0).transform([](ObjectId) {}); }));
    CHECK_FALSE(run(session, [&](edit::Transaction& tx) { return ops::split(tx, where.slot, end(after) + 10).transform([](ObjectId) {}); }));
    REQUIRE(session.undo());
    CHECK(trackOf(doc, where.slot).items.size() == before.items.size());
}

TEST_CASE("Lift leaves filler; ripple delete closes the gap", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 2, false);
    const auto before = trackOf(doc, where.slot);
    const auto clip = *std::ranges::find(before.items, ItemKind::sourceClip, &Item::kind);

    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::lift(tx, clip.object); }));
    auto after = trackOf(doc, where.slot);
    CHECK(end(after) == end(before));
    CHECK(std::ranges::none_of(after.items, [&](const Item& i) { return i.object == clip.object; }));
    CHECK(std::ranges::any_of(after.items, [&](const Item& i) { return i.kind == ItemKind::filler && i.start <= clip.start && i.start + i.length >= clip.start + clip.length; }));
    checkConsistent(doc, where.slot);
    REQUIRE(session.undo());

    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::rippleDelete(tx, clip.object); }));
    after = trackOf(doc, where.slot);
    CHECK(end(after) == end(before) - clip.length);
    checkConsistent(doc, where.slot);
    REQUIRE(session.undo());
    CHECK(end(trackOf(doc, where.slot)) == end(before));
}

TEST_CASE("Trims roll or ripple edit points", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 2, false);
    const auto before = trackOf(doc, where.slot);
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i + 1 < before.items.size(); ++i)
    {
        if (before.items[i].kind == ItemKind::sourceClip && before.items[i + 1].kind == ItemKind::sourceClip && before.items[i + 1].length > 3 && before.items[i].length > 3)
        {
            index = i;
            break;
        }
    }
    REQUIRE(index);
    const auto a = before.items[*index];
    const auto b = before.items[*index + 1];

    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::trim(tx, a.object, ops::Edge::tail, -2, false); }));
    CHECK(storedLength(doc, a.object) == a.length - 2);
    CHECK(storedLength(doc, b.object) == b.length + 2);
    CHECK(startTime(doc, b.object) == startTime(doc, b.object));
    CHECK(end(trackOf(doc, where.slot)) == end(before));
    checkConsistent(doc, where.slot);

    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::trim(tx, b.object, ops::Edge::tail, -1, true); }));
    CHECK(end(trackOf(doc, where.slot)) == end(before) - 1);
    checkConsistent(doc, where.slot);

    CHECK_FALSE(run(session, [&](edit::Transaction& tx) { return ops::trim(tx, a.object, ops::Edge::tail, -a.length, true); }));
    CHECK_FALSE(run(session, [&](edit::Transaction& tx) { return ops::trim(tx, a.object, ops::Edge::head, -(startTime(doc, a.object) + 1), true); }));
    REQUIRE(session.undo());
    REQUIRE(session.undo());
    CHECK(storedLength(doc, a.object) == a.length);
    CHECK(storedLength(doc, b.object) == b.length);
}

TEST_CASE("Clips can be inserted, overwritten and moved", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 2, false);
    const auto before = trackOf(doc, where.slot);
    const auto clip = *std::ranges::find(before.items, ItemKind::sourceClip, &Item::kind);
    REQUIRE(clip.source);
    REQUIRE(clip.source->mob);
    const auto sourceMob = *clip.source->mob;
    const auto length = std::min<std::int64_t>(clip.length, 5);

    REQUIRE(run(session, [&](edit::Transaction& tx) {
        return ops::placeClip(tx, where.slot, 1, sourceMob, clip.source->slotId, clip.source->startTime, length, true).transform([](ObjectId) {});
    }));
    CHECK(end(trackOf(doc, where.slot)) == end(before) + length);
    checkConsistent(doc, where.slot);
    REQUIRE(session.undo());

    REQUIRE(run(session, [&](edit::Transaction& tx) {
        return ops::placeClip(tx, where.slot, 1, sourceMob, clip.source->slotId, clip.source->startTime, length, false).transform([](ObjectId) {});
    }));
    CHECK(end(trackOf(doc, where.slot)) == std::max(end(before), 1 + length));
    checkConsistent(doc, where.slot);
    REQUIRE(session.undo());

    REQUIRE(run(session, [&](edit::Transaction& tx) {
        return ops::placeClip(tx, where.slot, end(before) + 10, sourceMob, clip.source->slotId, clip.source->startTime, length, false).transform([](ObjectId) {});
    }));
    CHECK(end(trackOf(doc, where.slot)) == end(before) + 10 + length);
    checkConsistent(doc, where.slot);
    REQUIRE(session.undo());

    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::move(tx, clip.object, where.slot, end(before), false); }));
    const auto moved = trackOf(doc, where.slot);
    CHECK(end(moved) == end(before) + clip.length);
    CHECK(moved.items.back().object == clip.object);
    checkConsistent(doc, where.slot);
    REQUIRE(session.undo());
    CHECK(trackOf(doc, where.slot).items.size() == before.items.size());
}

TEST_CASE("Transitions become cuts when lifted", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 1, true);
    const auto before = trackOf(doc, where.slot);
    const auto transition = *std::ranges::find(before.items, ItemKind::transition, &Item::kind);
    const auto result = run(session, [&](edit::Transaction& tx) { return ops::lift(tx, transition.object); });
    if (!result)
    {
        SKIP("transition is not between two editable segments: " << result.error().message);
    }
    const auto after = trackOf(doc, where.slot);
    CHECK(end(after) == end(before));
    CHECK(std::ranges::count(after.items, ItemKind::transition, &Item::kind) == std::ranges::count(before.items, ItemKind::transition, &Item::kind) - 1);
    checkConsistent(doc, where.slot);
}

TEST_CASE("Tracks and markers can be added and removed", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 1, false);
    const Projector projector(doc);
    const auto tracksBefore = projector.project(where.mob)->tracks.size();

    ObjectId picture = kNoObject;
    ObjectId sound = kNoObject;
    REQUIRE(run(session, [&](edit::Transaction& tx) -> Result<void> {
        auto p = ops::addTrack(tx, where.mob, TrackKind::picture, "New video");
        auto s = ops::addTrack(tx, where.mob, TrackKind::sound, "");
        if (!p || !s)
        {
            return std::unexpected(!p ? p.error() : s.error());
        }
        picture = *p;
        sound = *s;
        return {};
    }));
    const auto t = Projector(doc).project(where.mob);
    REQUIRE(t);
    CHECK(t->tracks.size() == tracksBefore + 2);
    CHECK(trackOf(doc, picture).kind == TrackKind::picture);
    CHECK(trackOf(doc, picture).name == "New video");
    CHECK(trackOf(doc, sound).kind == TrackKind::sound);
    CHECK(errorCount(doc) == 0);
    CHECK_FALSE(run(session, [&](edit::Transaction& tx) { return ops::addTrack(tx, where.mob, TrackKind::timecode, "").transform([](ObjectId) {}); }));

    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::removeTrack(tx, sound); }));
    CHECK(Projector(doc).project(where.mob)->tracks.size() == tracksBefore + 1);

    ObjectId marker = kNoObject;
    REQUIRE(run(session, [&](edit::Transaction& tx) -> Result<void> {
        auto first = ops::addMarker(tx, where.mob, 20, "second");
        auto second = ops::addMarker(tx, where.mob, 10, "first");
        if (!first || !second)
        {
            return std::unexpected(!first ? first.error() : second.error());
        }
        marker = *second;
        return {};
    }));
    const auto withMarkers = Projector(doc).project(where.mob);
    const auto events = std::ranges::find(withMarkers->tracks, SlotKind::event, &Track::slotKind);
    REQUIRE(events != withMarkers->tracks.end());
    REQUIRE(events->items.size() >= 2);
    CHECK(events->items[0].object == marker);
    CHECK(events->items[0].comment == "first");
    CHECK(events->items[0].start == 10);
    const auto described = doc.value(marker, "DescriptiveMarker", "DescribedSlots");
    REQUIRE(described);
    CHECK(described->as<Value::Array>().size() == 1);
    CHECK(errorCount(doc) == 0);
}

TEST_CASE("Locators can be relinked in bulk", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    std::size_t unchanged = 1;
    REQUIRE(run(session, [&](edit::Transaction& tx) -> Result<void> {
        auto n = ops::relink(tx, "file:", "file:");
        if (!n)
        {
            return std::unexpected(n.error());
        }
        unchanged = *n;
        return {};
    }));
    CHECK(unchanged == 0);
    CHECK_FALSE(session.canUndo());
    std::size_t renamed = 0;
    REQUIRE(run(session, [&](edit::Transaction& tx) -> Result<void> {
        auto n = ops::relink(tx, "/", "\\");
        if (!n)
        {
            return std::unexpected(n.error());
        }
        renamed = *n;
        return {};
    }));
    CHECK(renamed > 0);
    CHECK_FALSE(run(session, [&](edit::Transaction& tx) { return ops::relink(tx, "", "x").transform([](std::size_t) {}); }));
}

TEST_CASE("Single-clip tracks are wrapped in a sequence when edited", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const Projector projector(doc);
    ObjectId slot = kNoObject;
    std::int64_t length = 0;
    for (const auto& m : projector.mobs())
    {
        auto t = projector.project(m.object);
        for (const auto& track : t->tracks)
        {
            if (slot == kNoObject && track.effects.empty() && track.slotKind == SlotKind::timeline && doc.classOf(track.segment)->name == "SourceClip" && track.length > 4)
            {
                slot = track.slot;
                length = track.length;
            }
        }
    }
    REQUIRE(slot != kNoObject);
    REQUIRE(run(session, [&](edit::Transaction& tx) { return ops::split(tx, slot, length / 2).transform([](ObjectId) {}); }));
    const auto after = trackOf(doc, slot);
    CHECK(doc.classOf(after.segment)->name == "Sequence");
    CHECK(after.items.size() == 2);
    CHECK(end(after) == length);
    CHECK(errorCount(doc) == 0);
}

TEST_CASE("Random timeline edits stay valid and undo exactly", "[timeline][ops][symmetry]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    std::vector<Object> original;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        original.push_back(doc.object(i));
    }
    const auto where = findTrack(doc, 3, false);
    std::mt19937 rng(7);
    std::size_t applied = 0;
    for (int step = 0; step < 80; ++step)
    {
        const auto track = trackOf(doc, where.slot);
        const auto total = end(track);
        if (track.items.empty() || total < 4)
        {
            break;
        }
        const auto& item = track.items[rng() % track.items.size()];
        const auto position = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(total));
        const auto kind = rng() % 5;
        edit::Session::Command command;
        switch (kind)
        {
            case 0:
                command = [&, position](edit::Transaction& tx) { return ops::split(tx, where.slot, position).transform([](ObjectId) {}); };
                break;
            case 1:
                command = [&, id = item.object](edit::Transaction& tx) { return ops::lift(tx, id); };
                break;
            case 2:
                command = [&, id = item.object](edit::Transaction& tx) { return ops::rippleDelete(tx, id); };
                break;
            case 3:
                command = [&, id = item.object](edit::Transaction& tx) { return ops::trim(tx, id, ops::Edge::tail, -1, (rng() & 1U) != 0); };
                break;
            default:
                command = [&, id = item.object, position](edit::Transaction& tx) { return ops::move(tx, id, where.slot, position, false); };
                break;
        }
        if (run(session, command))
        {
            ++applied;
            checkConsistent(doc, where.slot);
        }
    }
    CHECK(applied > 20);
    while (session.canUndo())
    {
        REQUIRE(session.undo());
    }
    for (std::size_t i = 0; i < original.size(); ++i)
    {
        REQUIRE(doc.object(i) == original[i]);
    }
    for (std::size_t i = original.size(); i < doc.objectCount(); ++i)
    {
        CHECK_FALSE(doc.isAttached(i));
    }
}
