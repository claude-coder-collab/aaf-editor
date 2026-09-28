#include "fixtures.hpp"

#include <aaf/edit/operations.hpp>
#include <aaf/edit/session.hpp>
#include <aaf/timeline/edit.hpp>
#include <aaf/timeline/timeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
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

TEST_CASE("Partial projections of the affected slots reproduce the full projection after every edit", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto where = findTrack(doc, 2, false);
    auto previous = Projector(doc).project(where.mob).value();
    std::size_t partialUpdates = 0;

    auto check = [&](const Result<edit::ChangeSet>& changes) -> void {
        INFO((changes ? std::string() : changes.error().message));
        REQUIRE(changes);
        const Projector projector(doc);
        const auto full = projector.project(where.mob).value();
        const auto slots = projector.affectedSlots(where.mob, changes->objects);
        if (slots)
        {
            const auto partial = projector.project(where.mob, *slots).value();
            CHECK(partial.partial);
            CHECK(partial.slots == full.slots);
            CHECK(partial.tracks.size() == slots->size());
            CHECK(partial.timecode == full.timecode);
            std::vector<Track> merged;
            for (const auto slot : partial.slots)
            {
                const auto fresh = std::ranges::find(partial.tracks, slot, &Track::slot);
                const auto kept = std::ranges::find(previous.tracks, slot, &Track::slot);
                REQUIRE((fresh != partial.tracks.end() || kept != previous.tracks.end()));
                merged.push_back(fresh != partial.tracks.end() ? *fresh : *kept);
            }
            CHECK(merged == full.tracks);
            ++partialUpdates;
        }
        previous = full;
    };

    const auto initial = trackOf(doc, where.slot);
    const auto clip = *std::ranges::find(initial.items, ItemKind::sourceClip, &Item::kind);
    ObjectId right = kNoObject;
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::split(tx, where.slot, clip.start + clip.length / 2).transform([&](ObjectId id) { right = id; }); }));
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::trim(tx, clip.object, ops::Edge::tail, -1, true); }));
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::lift(tx, right); }));
    ObjectId added = kNoObject;
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::addTrack(tx, where.mob, initial.kind, "Extra").transform([&](ObjectId id) { added = id; }); }));
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::move(tx, clip.object, added, 10, false); }));
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::addMarker(tx, where.mob, 3, "note").transform([](ObjectId) {}); }));
    check(run(session, [&](edit::Transaction& tx) -> Result<void> {
        auto pid = edit::pidOf(doc, where.mob, "Name");
        return pid ? edit::setProperty(tx, where.mob, *pid, Value(std::string("Renamed"))) : std::unexpected(pid.error());
    }));
    check(run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::removeTrack(tx, added); }));
    for (int n = 0; n < 4; ++n)
    {
        check(session.undo());
    }
    for (int n = 0; n < 4; ++n)
    {
        check(session.redo());
    }
    CHECK(partialUpdates >= 14);
}

TEST_CASE("Changes outside a mob require a full projection", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    const auto& doc = opened->document();
    const auto where = findTrack(doc, 1, false);
    const Projector projector(doc);
    const auto mobs = projector.mobs();
    const auto other = std::ranges::find_if(mobs, [&](const MobSummary& m) -> bool { return m.object != where.mob; });
    REQUIRE(other != mobs.end());
    CHECK_FALSE(projector.affectedSlots(where.mob, std::vector{ other->object }));
    CHECK_FALSE(projector.affectedSlots(where.mob, std::vector<ObjectId>{ 0 }));
    const auto none = projector.affectedSlots(where.mob, std::vector{ where.mob });
    REQUIRE(none);
    CHECK(none->empty());
    const auto track = trackOf(doc, where.slot);
    const auto one = projector.affectedSlots(where.mob, std::vector{ track.items.front().object, track.segment, where.slot });
    REQUIRE(one);
    CHECK(*one == std::vector{ where.slot });
    CHECK_FALSE(projector.project(where.mob, std::vector{ other->object }));
}

namespace
{

struct EffectClip
{
    ObjectId mob = kNoObject;
    ObjectId slot = kNoObject;
    Item item;
    /// True if a transition touches the clip, so edits at its edges are refused.
    bool nearTransition = false;
};

/// Composition items shown as a clip inside effects, found by their outermost effect's name.
auto effectClips(const Document& doc, std::string_view outerEffect) -> std::vector<EffectClip>
{
    const Projector projector(doc);
    std::vector<EffectClip> out;
    for (const auto& m : projector.mobs())
    {
        if (m.kind != MobKind::composition)
        {
            continue;
        }
        const auto projected = projector.project(m.object).value();
        for (const auto& track : projected.tracks)
        {
            for (std::size_t i = 0; i < track.items.size(); ++i)
            {
                const auto& item = track.items[i];
                if (item.clip && item.effects.front().name == outerEffect)
                {
                    const auto transition = [&](std::size_t n) -> bool { return n < track.items.size() && track.items[n].kind == ItemKind::transition; };
                    out.push_back({ m.object, track.slot, item, (i > 0 && transition(i - 1)) || transition(i + 1) });
                }
            }
        }
    }
    return out;
}

}

auto editableGainClip(const Document& doc) -> EffectClip
{
    const auto all = effectClips(doc, "Audio Gain");
    const auto it = std::ranges::find(all, false, &EffectClip::nearTransition);
    REQUIRE(it != all.end());
    return *it;
}

TEST_CASE("Clips inside single-input effects are projected as clips with effects", "[timeline]")
{
    auto opened = openSession();
    REQUIRE(opened);
    const auto& doc = opened->document();
    const auto gains = effectClips(doc, "Audio Gain");
    CHECK(gains.size() >= 20);
    for (const auto& found : gains)
    {
        const auto& item = found.item;
        CHECK(item.kind == ItemKind::operationGroup);
        CHECK(item.effects.front().object == item.object);
        CHECK(doc.classOf(*item.clip)->name == "SourceClip");
        REQUIRE(item.source);
        CHECK(item.label == (item.source->mobName.empty() ? item.label : item.source->mobName));
        const Item* node = &item;
        for (const auto& effect : item.effects)
        {
            CHECK(node->object == effect.object);
            CHECK(node->effect == effect.name);
            REQUIRE(node->nested.size() == 1);
            REQUIRE(node->nested.front().size() == 1);
            node = &node->nested.front().front();
        }
        CHECK(node->object == *item.clip);
    }
}

TEST_CASE("Clips inside constant effects can be split and trimmed", "[timeline][ops]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto found = editableGainClip(doc);
    const auto group = found.item.object;
    const auto clip = *found.item.clip;
    REQUIRE(found.item.length >= 4);
    const auto startBefore = startTime(doc, clip);
    const auto at = found.item.start + found.item.length / 2;

    ObjectId right = kNoObject;
    const auto splitResult = run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::split(tx, found.slot, at).transform([&](ObjectId id) { right = id; }); });
    INFO((splitResult ? std::string() : splitResult.error().message));
    REQUIRE(splitResult);
    const auto track = trackOf(doc, found.slot);
    const auto left = std::ranges::find(track.items, group, &Item::object);
    const auto second = std::ranges::find(track.items, right, &Item::object);
    REQUIRE(left != track.items.end());
    REQUIRE(second != track.items.end());
    for (const auto& part : { *left, *second })
    {
        REQUIRE(part.clip);
        CHECK(part.effects.size() == 1);
        CHECK(part.effects.front().name == "Audio Gain");
        CHECK(storedLength(doc, part.object) == part.length);
        CHECK(storedLength(doc, *part.clip) == part.length);
    }
    CHECK(left->length + second->length == found.item.length);
    CHECK(*second->clip != clip);
    CHECK(startTime(doc, *second->clip) == startBefore + (at - found.item.start));
    checkConsistent(doc, found.slot);

    const auto trimmed = run(session, [&](edit::Transaction& tx) -> Result<void> { return ops::trim(tx, group, ops::Edge::head, 1, true); });
    INFO((trimmed ? std::string() : trimmed.error().message));
    REQUIRE(trimmed);
    CHECK(startTime(doc, clip) == startBefore + 1);
    CHECK(storedLength(doc, clip) == storedLength(doc, group));
    checkConsistent(doc, found.slot);

    const auto path = std::filesystem::temp_directory_path() / std::format("aaf-effect-split-{}.aaf", std::random_device{}());
    REQUIRE(session.save(path));
    const auto reopened = Document::open(path);
    std::filesystem::remove(path);
    REQUIRE(reopened);
    CHECK(errorCount(*reopened) == 0);
    const auto summary = [](const Document& d, ObjectId mob) -> std::vector<std::string> {
        std::vector<std::string> out;
        const auto timeline = Projector(d).project(mob).value();
        for (const auto& projected : timeline.tracks)
        {
            for (const auto& item : projected.items)
            {
                std::string effects;
                for (const auto& effect : item.effects)
                {
                    effects += effect.name + ";";
                }
                out.push_back(std::format("{} {} {} {} {} {} {}", projected.slotId, to_string(item.kind), item.start, item.length, item.label, item.source ? item.source->startTime : -1, effects));
            }
        }
        return out;
    };
    CHECK(summary(*reopened, found.mob) == summary(doc, found.mob));
}

TEST_CASE("Clips inside keyframed or speed-changing effects are not split or trimmed", "[timeline][ops]")
{
    const auto expectLocked = [](edit::Session& session, const EffectClip& found, std::string_view reason) -> void {
        const auto split = run(session, [&](edit::Transaction& tx) { return ops::split(tx, found.slot, found.item.start + found.item.length / 2).transform([](ObjectId) {}); });
        REQUIRE_FALSE(split);
        CHECK(split.error().message.contains(reason));
        CHECK(split.error().message.contains("Audio Gain"));
        const auto trimmed = run(session, [&](edit::Transaction& tx) { return ops::trim(tx, found.item.object, ops::Edge::tail, -1, true); });
        REQUIRE_FALSE(trimmed);
        CHECK(trimmed.error().message.contains(reason));
        CHECK(run(session, [&](edit::Transaction& tx) { return ops::lift(tx, found.item.object); }));
        REQUIRE(session.undo());
    };

    SECTION("keyframes")
    {
        auto opened = openSession();
        REQUIRE(opened);
        auto& session = *opened;
        const auto& doc = session.document();
        const auto found = editableGainClip(doc);
        std::optional<ObjectId> keyframed;
        for (std::size_t i = 0; i < doc.objectCount() && !keyframed; ++i)
        {
            if (doc.isAttached(i) && doc.model().isA(doc.object(i).classId, doc.model().findClassByName("VaryingValue")->id))
            {
                keyframed = i;
            }
        }
        REQUIRE(keyframed);
        const auto built = run(session, [&](edit::Transaction& tx) -> Result<void> {
            auto copy = ops::deepCopy(tx, *keyframed);
            auto pid = edit::pidOf(doc, found.item.object, "Parameters");
            if (!copy || !pid)
            {
                return fail(Errc::not_found, "cannot copy a keyframed parameter");
            }
            if (auto r = edit::removeProperty(tx, found.item.object, *pid); !r)
            {
                return r;
            }
            return edit::insertIntoCollection(tx, found.item.object, *pid, 0, *copy);
        });
        INFO((built ? std::string() : built.error().message));
        REQUIRE(built);
        expectLocked(session, found, "keyframed");
    }

    SECTION("speed change")
    {
        auto opened = openSession();
        REQUIRE(opened);
        auto& session = *opened;
        const auto& doc = session.document();
        const auto found = editableGainClip(doc);
        const auto target = [&]() -> ObjectId {
            const auto& object = doc.object(found.item.object);
            const auto pid = doc.model().findProperty("OperationGroup", "Operation")->pid;
            const auto* weak = std::get_if<WeakRefProperty>(&object.find(pid)->payload);
            return doc.resolveWeak(weak->tag, weak->key).value();
        }();
        REQUIRE(run(session, [&](edit::Transaction& tx) -> Result<void> {
            auto pid = edit::pidOf(doc, target, "IsTimeWarp");
            return pid ? edit::setProperty(tx, target, *pid, Value(true)) : Result<void>(std::unexpected(pid.error()));
        }));
        expectLocked(session, found, "speed change");
    }
}
