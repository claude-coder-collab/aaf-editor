#include "fixtures.hpp"

#include <aaf/edit/operations.hpp>
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
constexpr std::string_view kSplitMono = "protools/split_mono_wav.aaf";

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

auto track(const MobTimeline& t, std::string_view name) -> const Track&
{
    const auto it = std::ranges::find_if(t.tracks, [&](const Track& tr) { return tr.name == name; });
    REQUIRE(it != t.tracks.end());
    return *it;
}

auto clips(const Track& t) -> std::vector<Item>
{
    std::vector<Item> out;
    std::ranges::copy_if(t.items, std::back_inserter(out), [](const Item& i) { return i.kind != ItemKind::filler; });
    return out;
}

auto errorCount(const Document& doc) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count(validate(doc), Diagnostic::Severity::error, &Diagnostic::severity));
}

auto integer(const Document& doc, ObjectId id, std::string_view cls, std::string_view name) -> std::int64_t
{
    const auto v = doc.value(id, cls, name);
    REQUIRE(v);
    return v->is<std::int64_t>() ? v->as<std::int64_t>() : static_cast<std::int64_t>(v->as<std::uint64_t>());
}

}

TEST_CASE("Track formats come from _TRACK_FORMAT", "[timeline][multichannel]")
{
    auto session = openSession(kFrameAligned);
    const auto t = composition(session.document());
    CHECK(track(t, "mono").channels == 0);
    CHECK(track(t, "stereo").channels == 2);
    CHECK(track(t, "5.1").channels == 6);
    CHECK(track(t, "7.1").channels == 8);
    CHECK(channelFormatName(0).empty());
    CHECK(channelFormatName(2) == "Stereo");
    CHECK(channelFormatName(6) == "5.1");
    CHECK(channelFormatName(8) == "7.1");
    CHECK(channelFormatName(4) == "4 channels");
}

TEST_CASE("Channel combiners project as one multichannel clip", "[timeline][multichannel]")
{
    auto session = openSession(kFrameAligned);
    const auto t = composition(session.document());

    const auto stereo = clips(track(t, "stereo"));
    REQUIRE(stereo.size() == 3);
    CHECK(stereo[0].label == "stereo_01-01");
    CHECK(stereo[1].label == "Fade ");
    CHECK(stereo[2].label == "stereo_02-01");
    for (const auto& item : stereo)
    {
        CHECK(item.channels.size() == 2);
        CHECK(item.clip == item.object);
        CHECK(item.effects.empty());
        REQUIRE(item.source);
        CHECK(item.source->mob.has_value());
    }

    const auto surround = clips(track(t, "7.1"));
    REQUIRE(surround.size() == 1);
    CHECK(surround[0].label == "7.1_01-01");
    CHECK(surround[0].channels.size() == 8);
    CHECK(surround[0].length == 7);

    for (const auto& item : clips(track(t, "mono")))
    {
        CHECK(item.channels.empty());
    }
}

TEST_CASE("Split-to-mono exports have no multichannel clips", "[timeline][multichannel]")
{
    auto session = openSession(kSplitMono);
    const auto t = composition(session.document());
    CHECK(std::ranges::count_if(t.tracks, [](const Track& tr) { return tr.name == "5.1"; }) == 6);
    for (const auto& tr : t.tracks)
    {
        CHECK(tr.channels == 0);
        CHECK(std::ranges::none_of(tr.items, [](const Item& i) { return !i.channels.empty(); }));
    }
}

TEST_CASE("Splitting a multichannel clip splits every channel", "[timeline][multichannel][edit]")
{
    auto session = openSession(kFrameAligned);
    const auto& doc = session.document();
    const auto before = composition(doc);
    const auto& surround = track(before, "7.1");
    const auto original = clips(surround).front();
    std::vector<std::int64_t> starts;
    for (const auto channel : original.channels)
    {
        starts.push_back(integer(doc, channel, "SourceClip", "StartTime"));
    }

    REQUIRE(session.execute("split", [&](edit::Transaction& tx) -> Result<void> {
        auto right = ops::split(tx, surround.slot, original.start + 3);
        return right ? Result<void>{} : Result<void>(std::unexpected(right.error()));
    }));
    CHECK(errorCount(doc) == 0);
    const auto parts = clips(track(composition(doc), "7.1"));
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].length == 3);
    CHECK(parts[1].length == 4);
    for (std::size_t c = 0; c < 8; ++c)
    {
        REQUIRE(parts[0].channels.size() == 8);
        REQUIRE(parts[1].channels.size() == 8);
        CHECK(integer(doc, parts[0].channels[c], "Component", "Length") == 3);
        CHECK(integer(doc, parts[1].channels[c], "Component", "Length") == 4);
        CHECK(integer(doc, parts[0].channels[c], "SourceClip", "StartTime") == starts[c]);
        CHECK(integer(doc, parts[1].channels[c], "SourceClip", "StartTime") == starts[c] + 3);
    }
    CHECK(parts[1].label == "7.1_01-01");
}

TEST_CASE("Trimming a multichannel clip trims every channel", "[timeline][multichannel][edit]")
{
    auto session = openSession(kSampleAccurate);
    const auto& doc = session.document();
    const auto stereo = clips(track(composition(doc), "stereo"));
    const auto target = *std::ranges::find_if(stereo, [](const Item& i) { return i.length == 2; });

    REQUIRE(session.execute("trim", [&](edit::Transaction& tx) { return ops::trim(tx, target.object, ops::Edge::tail, -1, true); }));
    CHECK(errorCount(doc) == 0);
    CHECK(integer(doc, target.object, "Component", "Length") == 1);
    for (const auto channel : target.channels)
    {
        CHECK(integer(doc, channel, "Component", "Length") == 1);
    }

    REQUIRE(session.execute("head", [&](edit::Transaction& tx) { return ops::trim(tx, target.object, ops::Edge::head, -1, true); }));
    CHECK(errorCount(doc) == 0);
    for (const auto channel : target.channels)
    {
        CHECK(integer(doc, channel, "Component", "Length") == 2);
    }
}

TEST_CASE("A channel whose length differs from its combiner is an error", "[timeline][multichannel][validate]")
{
    auto session = openSession(kFrameAligned);
    const auto& doc = session.document();
    const auto item = clips(track(composition(doc), "stereo")).front();
    const auto channel = item.channels.front();
    const auto rejected = session.execute("corrupt", [&](edit::Transaction& tx) -> Result<void> {
        auto pid = edit::pidOf(tx.document(), channel, "Length");
        return pid ? edit::setProperty(tx, channel, *pid, Value(std::int64_t{ 1 })) : Result<void>(std::unexpected(pid.error()));
    });
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.contains("channel combiner"));
    CHECK(errorCount(doc) == 0);
}

TEST_CASE("Every Pro Tools export validates and round-trips", "[timeline][multichannel]")
{
    for (const auto& file : aafFilesIn("protools"))
    {
        INFO(file.string());
        auto doc = Document::open(file);
        REQUIRE(doc);
        CHECK(errorCount(*doc) == 0);
    }
    CHECK(aafFilesIn("protools").size() == 5);
}
