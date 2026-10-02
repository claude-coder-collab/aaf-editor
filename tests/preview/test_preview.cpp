#include "fixtures.hpp"

#include <aaf/edit/operations.hpp>
#include <aaf/edit/session.hpp>
#include <aaf/preview/preview.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>

using namespace aaf;
using namespace aaf::preview;

namespace
{

auto samplePath() -> std::filesystem::path
{
    return test::fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf";
}

auto count(std::string_view text, std::string_view needle) -> std::size_t
{
    std::size_t n = 0;
    for (auto pos = text.find(needle); pos != std::string_view::npos; pos = text.find(needle, pos + needle.size()))
    {
        ++n;
    }
    return n;
}

}

TEST_CASE("Timecodes are formatted like SMPTE 12M", "[preview]")
{
    CHECK(formatTimecode(0, 25, false) == "00:00:00:00");
    CHECK(formatTimecode(25 * 3600 + 25 * 61 + 3, 25, false) == "01:01:01:03");
    CHECK(formatTimecode(-26, 25, false) == "-00:00:01:01");
    CHECK(formatTimecode(1800, 30, true) == "00:01:00;02");
    CHECK(formatTimecode(17982, 30, true) == "00:10:00;00");
    CHECK(formatTimecode(1799, 30, true) == "00:00:59;29");
    CHECK(formatTimecode(10, 0, false) == "--:--:--:--");
}

TEST_CASE("Text is escaped for HTML", "[preview]")
{
    CHECK(escapeHtml(R"(<a href="x">Tom & Jerry's</a>)") == "&lt;a href=&quot;x&quot;&gt;Tom &amp; Jerry&#39;s&lt;/a&gt;");
}

TEST_CASE("The preview summarises the file", "[preview]")
{
    const auto html = previewFile(samplePath());
    CHECK(html.starts_with("<!DOCTYPE html>"));
    CHECK(html.contains("RealWorldSample1.aaf"));
    CHECK(html.contains("Avid Xpress Pro HD 5.2.4"));
    CHECK_FALSE(html.contains("Unknown version"));
    CHECK(html.contains("LUCID(a)"));
    CHECK(html.contains("MUSIC PT1"));
    CHECK(html.contains("Master clips"));
    CHECK(html.contains("00:00:33:21"));
    CHECK_FALSE(html.contains("<script"));
    CHECK_FALSE(html.contains("could not be previewed"));
    CHECK(count(html, "<div") == count(html, "</div>"));
    CHECK(count(html, "<table") == count(html, "</table>"));
}

TEST_CASE("Long lists are truncated with a count", "[preview]")
{
    auto doc = Document::open(samplePath());
    REQUIRE(doc);
    const auto html = renderPreview(*doc, { .fileName = "x.aaf", .fileSize = std::nullopt, .maxClips = 1, .maxMobs = 2 });
    CHECK(html.contains("and 1 more clips"));
    CHECK(html.contains("and 23 more"));
    CHECK_FALSE(html.contains("<dt>Size</dt>"));
}

TEST_CASE("Names from the file are escaped", "[preview]")
{
    auto session = edit::Session::open(samplePath());
    REQUIRE(session);
    const auto& doc = session->document();
    ObjectId mob = kNoObject;
    for (std::size_t i = 0; i < doc.objectCount() && mob == kNoObject; ++i)
    {
        if (const auto* cls = doc.classOf(i); cls != nullptr && cls->name == "MasterMob" && doc.isAttached(i))
        {
            mob = i;
        }
    }
    REQUIRE(mob != kNoObject);
    REQUIRE(session->execute("rename", [&](edit::Transaction& tx) -> Result<void> {
        auto pid = edit::pidOf(tx.document(), mob, "Name");
        return pid ? edit::setProperty(tx, mob, *pid, Value(std::string("<img src=x onerror=alert(1)>"))) : Result<void>(std::unexpected(pid.error()));
    }));
    const auto html = renderPreview(doc, { .fileName = "<evil>.aaf", .fileSize = std::nullopt });
    CHECK_FALSE(html.contains("<img"));
    CHECK_FALSE(html.contains("<evil>"));
    CHECK(html.contains("&lt;img src=x onerror=alert(1)&gt;"));
}

TEST_CASE("Unreadable files give an error page", "[preview]")
{
    const auto path = std::filesystem::temp_directory_path() / std::format("aaf-preview-{}.aaf", std::random_device{}());
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not a compound file";
    }
    const auto html = previewFile(path);
    std::filesystem::remove(path);
    CHECK(html.contains("could not be previewed"));
    CHECK(previewFile("/nonexistent/file.aaf").contains("could not be previewed"));
}
