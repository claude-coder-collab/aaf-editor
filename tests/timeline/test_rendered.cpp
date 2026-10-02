#include "fixtures.hpp"

#include <aaf/edit/session.hpp>
#include <aaf/timeline/edit.hpp>
#include <aaf/timeline/timeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace aaf;
using namespace aaf::timeline;
using namespace aaf::test;

namespace
{

constexpr std::string_view kFrameAligned = "protools/multichannel_frame_aligned.aaf";
constexpr std::string_view kSampleAccurate = "protools/multichannel_wav.aaf";

auto openSession(std::string_view file) -> edit::Session
{
    auto s = edit::Session::open(fixturesDir() / file);
    REQUIRE(s);
    return std::move(*s);
}

auto composition(const Document& doc) -> MobTimeline
{
    const Projector projector(doc);
    const auto mobs = projector.mobs();
    const auto it = std::ranges::find_if(mobs, [](const MobSummary& m) { return m.kind == MobKind::composition; });
    REQUIRE(it != mobs.end());
    auto t = projector.project(it->object);
    REQUIRE(t);
    return std::move(*t);
}

auto track(const MobTimeline& t, std::string_view name) -> Track
{
    const auto it = std::ranges::find_if(t.tracks, [&](const Track& tr) { return tr.name == name; });
    REQUIRE(it != t.tracks.end());
    return *it;
}

auto named(const Track& t, std::string_view label) -> Item
{
    const auto it = std::ranges::find_if(t.items, [&](const Item& i) { return i.label == label; });
    REQUIRE(it != t.items.end());
    return *it;
}

auto rendered(const Track& t, Rendered role) -> Item
{
    const auto it = std::ranges::find_if(t.items, [&](const Item& i) { return i.rendered == role; });
    REQUIRE(it != t.items.end());
    return *it;
}

auto errorCount(const Document& doc) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count(validate(doc), Diagnostic::Severity::error, &Diagnostic::severity));
}

auto startTime(const Document& doc, ObjectId clip) -> std::int64_t
{
    return doc.value(clip, "SourceClip", "StartTime")->as<std::int64_t>();
}

}

TEST_CASE("Rendered fades and seams are classified by their neighbours", "[timeline][rendered]")
{
    {
        auto session = openSession(kFrameAligned);
        const auto t = composition(session.document());
        CHECK(named(track(t, "mono"), "Fade ").rendered == Rendered::crossfade);
        CHECK(named(track(t, "stereo"), "Fade ").rendered == Rendered::crossfade);
        CHECK(named(track(t, "5.1"), "Fade ").rendered == Rendered::fadeIn);
        CHECK(named(track(t, "mono"), "mono_01-02").rendered == Rendered::none);
        CHECK(to_string(Rendered::fadeIn) == "fadeIn");
    }
    {
        auto session = openSession(kSampleAccurate);
        const auto t = composition(session.document());
        const auto stereo = track(t, "stereo");
        CHECK(std::ranges::count(stereo.items, Rendered::seam, &Item::rendered) == 2);
        CHECK(std::ranges::count(stereo.items, Rendered::region, &Item::rendered) == 2);
    }
    {
        auto session = edit::Session::open(fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf");
        REQUIRE(session);
        const Projector projector(session->document());
        for (const auto& m : projector.mobs())
        {
            for (const auto& tr : projector.project(m.object)->tracks)
            {
                CHECK(std::ranges::all_of(tr.items, [](const Item& i) { return i.rendered == Rendered::none; }));
            }
        }
    }
}

TEST_CASE("Removing a crossfade gives its length to the clips around it", "[timeline][rendered][edit]")
{
    auto session = openSession(kFrameAligned);
    const auto& doc = session.document();
    const auto mono = track(composition(doc), "mono");
    const auto out = named(mono, "mono_01-02");
    const auto in = named(mono, "mono_01-03");
    const auto fade = named(mono, "Fade ");
    std::vector<std::string> warnings;
    REQUIRE(session.execute("remove fade", [&](edit::Transaction& tx) { return ops::removeFade(tx, fade.object, &warnings); }));
    CHECK(errorCount(doc) == 0);
    CHECK(warnings == std::vector<std::string>{ "Replaced a rendered crossfade with a cut." });
    const auto after = track(composition(doc), "mono");
    CHECK(after.length == mono.length);
    CHECK(std::ranges::none_of(after.items, [](const Item& i) { return i.label == "Fade "; }));
    const auto outAfter = named(after, "mono_01-02");
    const auto inAfter = named(after, "mono_01-03");
    CHECK(outAfter.length == out.length + fade.length - fade.length / 2);
    CHECK(inAfter.start == in.start - fade.length / 2);
    CHECK(outAfter.start + outAfter.length == inAfter.start);
}

TEST_CASE("Removing a multichannel fade-in extends every channel of its clip", "[timeline][rendered][edit][multichannel]")
{
    auto session = openSession(kFrameAligned);
    const auto& doc = session.document();
    const auto surround = track(composition(doc), "5.1");
    const auto fade = rendered(surround, Rendered::fadeIn);
    const auto clip = named(surround, "5.1_01-01");
    std::vector<std::int64_t> starts;
    for (const auto channel : clip.channels)
    {
        starts.push_back(startTime(doc, channel));
    }
    REQUIRE(session.execute("lift fade", [&](edit::Transaction& tx) { return ops::lift(tx, fade.object); }));
    CHECK(errorCount(doc) == 0);
    const auto after = named(track(composition(doc), "5.1"), "5.1_01-01");
    CHECK(after.start == fade.start);
    CHECK(after.length == clip.length + fade.length);
    for (std::size_t c = 0; c < after.channels.size(); ++c)
    {
        CHECK(startTime(doc, after.channels[c]) == starts[c] - fade.length);
    }
}

TEST_CASE("Lifting a clip replaces its crossfade with a cut first", "[timeline][rendered][edit]")
{
    auto session = openSession(kFrameAligned);
    const auto& doc = session.document();
    const auto stereo = track(composition(doc), "stereo");
    const auto incoming = named(stereo, "stereo_02-01");
    std::vector<std::string> warnings;
    REQUIRE(session.execute("lift", [&](edit::Transaction& tx) { return ops::lift(tx, incoming.object, &warnings); }));
    CHECK(errorCount(doc) == 0);
    CHECK(warnings.size() == 1);
    const auto after = track(composition(doc), "stereo");
    CHECK(after.length == stereo.length);
    CHECK(std::ranges::none_of(after.items, [](const Item& i) { return i.rendered != Rendered::none || i.label == "stereo_02-01"; }));
}

TEST_CASE("Trimming next to a fade keeps the requested edge", "[timeline][rendered][edit]")
{
    auto session = openSession(kFrameAligned);
    const auto& doc = session.document();
    const auto mono = track(composition(doc), "mono");
    const auto out = named(mono, "mono_01-02");
    const auto edge = out.start + out.length;
    REQUIRE(session.execute("trim", [&](edit::Transaction& tx) { return ops::trim(tx, out.object, ops::Edge::tail, -1, false); }));
    CHECK(errorCount(doc) == 0);
    const auto after = track(composition(doc), "mono");
    const auto outAfter = named(after, "mono_01-02");
    CHECK(outAfter.start + outAfter.length == edge - 1);
    CHECK(named(after, "mono_01-03").start == edge - 1);
    CHECK(after.length == mono.length);
}

TEST_CASE("Edits that would break rendered audio are refused", "[timeline][rendered][edit]")
{
    {
        auto session = openSession(kFrameAligned);
        const auto& doc = session.document();
        const auto mono = track(composition(doc), "mono");
        const auto out = named(mono, "mono_01-02");
        const auto split = session.execute("split", [&](edit::Transaction& tx) -> Result<void> {
            auto r = ops::split(tx, mono.slot, out.start + 1);
            return r ? Result<void>{} : Result<void>(std::unexpected(r.error()));
        });
        CHECK(split);
        CHECK(named(track(composition(doc), "mono"), "Fade ").rendered == Rendered::crossfade);
        const auto in = named(track(composition(doc), "mono"), "mono_01-03");
        const auto overwrite = session.execute("overwrite", [&](edit::Transaction& tx) -> Result<void> {
            auto r = ops::placeClip(tx, mono.slot, in.start, *in.source->mob, in.source->slotId, in.source->startTime, in.length, false);
            return r ? Result<void>{} : Result<void>(std::unexpected(r.error()));
        });
        REQUIRE_FALSE(overwrite);
        CHECK(overwrite.error().message.contains("rendered"));
        const auto ripple = session.execute("ripple", [&](edit::Transaction& tx) { return ops::rippleDelete(tx, named(track(composition(doc), "mono"), "Fade ").object); });
        CHECK_FALSE(ripple);
    }
    {
        auto session = openSession(kSampleAccurate);
        const auto& doc = session.document();
        const auto stereo = track(composition(doc), "stereo");
        const auto region = rendered(stereo, Rendered::region);
        const auto lifted = session.execute("lift", [&](edit::Transaction& tx) { return ops::lift(tx, region.object); });
        REQUIRE_FALSE(lifted);
        CHECK(lifted.error().message.contains("several frames"));
        const auto clip = named(stereo, "stereo_01-01");
        const auto trimmed = session.execute("trim", [&](edit::Transaction& tx) { return ops::trim(tx, clip.object, ops::Edge::tail, -1, true); });
        REQUIRE_FALSE(trimmed);
        CHECK(trimmed.error().message.contains("cannot be replaced by a cut"));
        CHECK(errorCount(doc) == 0);
    }
}
