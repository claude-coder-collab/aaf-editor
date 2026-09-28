#include "fixtures.hpp"

#include <aaf/edit/operations.hpp>
#include <aaf/edit/session.hpp>
#include <aaf/timeline/timeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

using namespace aaf;
using namespace aaf::timeline;
using namespace aaf::test;

namespace
{

auto firstOfKind(const Projector& projector, MobKind kind) -> MobSummary
{
    for (const auto& m : projector.mobs())
    {
        if (m.kind == kind)
        {
            return m;
        }
    }
    FAIL("no mob of the requested kind");
    return {};
}

auto firstComposition(const Projector& projector) -> MobSummary
{
    return firstOfKind(projector, MobKind::composition);
}

auto kindName(ItemKind kind) -> std::string
{
    switch (kind)
    {
        case ItemKind::filler:
            return "Gap";
        case ItemKind::transition:
            return "Transition";
        default:
            return "Clip";
    }
}

}

TEST_CASE("Rationals are exact and detect overflow", "[timeline][rational]")
{
    CHECK(Rational(50, 2) == Rational(25));
    CHECK(Rational(3, -6) == Rational(-1, 2));
    CHECK(Rational(30000, 1001).toString() == "30000/1001");
    CHECK(Rational(1, 3).add(Rational(1, 6)).value() == Rational(1, 2));
    CHECK(Rational(1, 3).subtract(Rational(1, 2)).value() == Rational(-1, 6));
    CHECK(Rational(2, 3).multiply(Rational(9, 4)).value() == Rational(3, 2));
    CHECK(Rational(2, 3).divide(Rational(4, 9)).value() == Rational(3, 2));
    CHECK_FALSE(Rational(1).divide(Rational(0)));
    constexpr auto big = std::numeric_limits<std::int64_t>::max();
    CHECK_FALSE(Rational(big).add(Rational(1)));
    CHECK_FALSE(Rational(big).multiply(Rational(2)));
    CHECK(Rational(big - 1, big) < Rational(big, big - 1));
    CHECK(Rational(-big, big - 1) < Rational(-big + 1, big));
    CHECK(Rational(1, 3) > Rational(1, 4));
    CHECK(Rational(0, 5) == Rational());
}

TEST_CASE("Positions convert between edit rates", "[timeline][rational]")
{
    CHECK(convertPosition(25, Rational(25), Rational(48000)).value() == 48000);
    CHECK(convertPosition(1001, Rational(30000, 1001), Rational(24000, 1001)).value() == 800);
    CHECK(convertPosition(1, Rational(48000), Rational(25)).value() == 0);
    CHECK(convertPosition(-1, Rational(48000), Rational(25)).value() == -1);
    CHECK_FALSE(convertPosition(1, Rational(0), Rational(25)));
}

TEST_CASE("Compositions project to tracks with contiguous items", "[timeline][projection]")
{
    const auto doc = Document::open(fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf");
    REQUIRE(doc);
    const Projector projector(*doc);
    const auto mobs = projector.mobs();
    REQUIRE(std::ranges::count(mobs, MobKind::composition, &MobSummary::kind) == 5);
    std::size_t checkedTracks = 0;
    for (const auto& m : mobs)
    {
        auto t = projector.project(m.object);
        REQUIRE(t);
        CHECK(t->warnings.empty());
        for (const auto& track : t->tracks)
        {
            CHECK(track.editRate > Rational(0));
            const bool hasTransition = std::ranges::any_of(track.items, [](const Item& i) { return i.kind == ItemKind::transition; });
            if (track.slotKind != SlotKind::timeline || hasTransition)
            {
                continue;
            }
            for (std::size_t i = 1; i < track.items.size(); ++i)
            {
                CHECK(track.items[i].start == track.items[i - 1].start + track.items[i - 1].length);
            }
            ++checkedTracks;
        }
    }
    CHECK(checkedTracks > 100);
    CHECK_FALSE(projector.project(Document::root()));
}

TEST_CASE("Transitions overlap their neighbours", "[timeline][projection][external]")
{
    const auto path = fixturesDir() / "external/otio-aaf-adapter/transitions.aaf";
    if (!std::filesystem::exists(path))
    {
        SKIP("external fixtures not fetched");
    }
    const auto doc = Document::open(path);
    REQUIRE(doc);
    const Projector projector(*doc);
    auto t = projector.project(firstComposition(projector).object);
    REQUIRE(t);
    std::size_t transitions = 0;
    for (const auto& track : t->tracks)
    {
        for (std::size_t i = 0; i + 1 < track.items.size(); ++i)
        {
            if (track.items[i].kind != ItemKind::transition)
            {
                continue;
            }
            ++transitions;
            CHECK(track.items[i + 1].start == track.items[i].start);
            if (i > 0)
            {
                CHECK(track.items[i].start + track.items[i].length == track.items[i - 1].start + track.items[i - 1].length);
            }
            CHECK_FALSE(track.items[i].effect.empty());
        }
    }
    CHECK(transitions >= 5);
    CHECK(t->timecode.has_value());
}

TEST_CASE("Projections match the OpenTimelineIO AAF adapter", "[timeline][projection][external]")
{
    const auto expectationsPath = fixturesDir() / "external/otio_expectations.json";
    if (!std::filesystem::exists(fixturesDir() / "external/otio-aaf-adapter"))
    {
        SKIP("external fixtures not fetched");
    }
    std::ifstream in(expectationsPath);
    const auto expectations = nlohmann::json::parse(in);
    std::size_t compared = 0;
    for (const auto& [file, expected] : expectations.items())
    {
        INFO(file);
        const auto doc = Document::open(fixturesDir() / "external/otio-aaf-adapter" / file);
        REQUIRE(doc);
        const Projector projector(*doc);
        const auto mobs = projector.mobs();
        const auto mob = std::ranges::find(mobs, expected["timeline"].get<std::string>(), &MobSummary::name);
        REQUIRE(mob != mobs.end());
        auto t = projector.project(mob->object);
        REQUIRE(t);
        std::vector<const Track*> tracks;
        bool referencesCompositions = false;
        for (const auto& track : t->tracks)
        {
            const bool media = track.kind == TrackKind::picture || track.kind == TrackKind::sound;
            const bool empty = std::ranges::all_of(track.items, [](const Item& i) { return i.kind == ItemKind::filler; });
            if (media && !empty)
            {
                tracks.push_back(&track);
                referencesCompositions = referencesCompositions || std::ranges::any_of(track.items, [](const Item& i) { return (i.source && i.source->mobKind == MobKind::composition) || i.kind == ItemKind::nestedScope; });
            }
        }
        std::vector<nlohmann::json> otioTracks;
        for (const auto& track : expected["tracks"])
        {
            if (track["kind"] == "Video" || track["kind"] == "Audio")
            {
                otioTracks.push_back(track);
            }
        }
        if (referencesCompositions)
        {
            continue;
        }
        REQUIRE(tracks.size() == otioTracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const auto& items = otioTracks[i]["items"];
            if (std::ranges::any_of(items, [](const nlohmann::json& x) { return x[0] != "Clip" && x[0] != "Gap"; }))
            {
                continue;
            }
            nlohmann::json ours = nlohmann::json::array();
            for (const auto& item : tracks[i]->items)
            {
                if (item.kind != ItemKind::marker && item.kind != ItemKind::event)
                {
                    ours.push_back({ kindName(item.kind), item.start, item.length });
                }
            }
            CHECK(ours == items);
            ++compared;
        }
    }
    CHECK(compared >= 30);
}

TEST_CASE("Source chains resolve to physical essence", "[timeline][resolve][external]")
{
    const auto path = fixturesDir() / "external/otio-aaf-adapter/picchu_seq0100_snippet_embedded.aaf";
    if (!std::filesystem::exists(path))
    {
        SKIP("external fixtures not fetched");
    }
    auto opened = edit::Session::open(path);
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    ObjectId clip = kNoObject;
    {
        const Projector projector(doc);
        auto t = projector.project(firstOfKind(projector, MobKind::master).object);
        REQUIRE(t);
        for (const auto& track : t->tracks)
        {
            for (const auto& item : track.items)
            {
                if (item.kind == ItemKind::sourceClip && item.source && item.source->mob && clip == kNoObject)
                {
                    clip = item.object;
                }
            }
        }
        REQUIRE(clip != kNoObject);
        auto chain = projector.resolve(clip);
        REQUIRE(chain);
        CHECK(chain->status == ChainStatus::resolved);
        REQUIRE_FALSE(chain->links.empty());
        CHECK(chain->links.front().mobKind == MobKind::source);
        REQUIRE(chain->essence);
        CHECK(chain->essence->embedded);
        CHECK(chain->essence->essenceData != kNoObject);
        CHECK_FALSE(projector.resolve(Document::root()));
    }

    REQUIRE(session.execute("Break source", [&](edit::Transaction& tx) {
        return edit::setProperty(tx, clip, edit::pidOf(doc, clip, "SourceID").value(), Value(MobId::generate()));
    }));
    const Projector broken(doc);
    auto chain = broken.resolve(clip);
    REQUIRE(chain);
    CHECK(chain->status == ChainStatus::missingMob);
}

TEST_CASE("External files with linked media report their locators", "[timeline][resolve][external]")
{
    const auto path = fixturesDir() / "external/otio-aaf-adapter/simple.aaf";
    if (!std::filesystem::exists(path))
    {
        SKIP("external fixtures not fetched");
    }
    const auto doc = Document::open(path);
    REQUIRE(doc);
    const Projector projector(*doc);
    auto t = projector.project(firstComposition(projector).object);
    REQUIRE(t);
    const auto& video = *std::ranges::find(t->tracks, TrackKind::picture, &Track::kind);
    const auto chain = projector.resolve(video.items.front().object);
    REQUIRE(chain);
    CHECK(chain->status == ChainStatus::resolved);
    CHECK(chain->links.back().mobKind == MobKind::source);
    REQUIRE(chain->essence);
    CHECK_FALSE(chain->essence->embedded);
    CHECK(video.items.front().label == "tech.fux (loop)-HD.mp4");
    const auto& audio = *std::ranges::find(t->tracks, TrackKind::sound, &Track::kind);
    CHECK((audio.effects.empty() || !audio.effects.front().name.empty()));
}
