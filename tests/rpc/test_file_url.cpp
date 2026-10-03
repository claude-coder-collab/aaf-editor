#include <aaf/rpc/file_url.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace aaf::rpc;

namespace
{

auto u8(std::string_view text) -> std::filesystem::path
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}

TEST_CASE("Local paths become file URLs", "[rpc][url]")
{
    CHECK(pathToFileUrl("/Users/me/My Edit.aaf") == "file:///Users/me/My%20Edit.aaf");
    CHECK(pathToFileUrl(u8("/tmp/caf\xC3\xA9 #1.aaf")) == "file:///tmp/caf%C3%A9%20%231.aaf");
    CHECK(pathToFileUrl("C:/Edits/a b.aaf") == "file:///C:/Edits/a%20b.aaf");
    CHECK(pathToFileUrl("//server/share/x.aaf") == "file://server/share/x.aaf");
}

TEST_CASE("File URLs become local paths", "[rpc][url]")
{
    CHECK(fileUrlToPath("file:///Users/me/My%20Edit.aaf") == u8("/Users/me/My Edit.aaf"));
    CHECK(fileUrlToPath("FILE://localhost/tmp/x.aaf") == u8("/tmp/x.aaf"));
    CHECK(fileUrlToPath("file:///C:/Edits/a%20b.aaf") == u8("C:/Edits/a b.aaf"));
    CHECK(fileUrlToPath("file://server/share/x.aaf") == u8("//server/share/x.aaf"));
    CHECK(fileUrlToPath("file:///tmp/caf%C3%A9%20%231.aaf?x#y") == u8("/tmp/caf\xC3\xA9 #1.aaf"));
    CHECK_FALSE(fileUrlToPath("https://example.com/x.aaf"));
    CHECK_FALSE(fileUrlToPath("file:///bad%2"));
    CHECK_FALSE(fileUrlToPath("file:///bad%zz"));
    CHECK_FALSE(fileUrlToPath("file://"));
}

TEST_CASE("Paths round-trip through file URLs", "[rpc][url]")
{
    for (const auto* text : { "/a/b c/d.aaf", "/x%y/z.aaf", "C:/a/b.aaf", "//host/share/f.aaf", "/\xE6\x97\xA5\xE6\x9C\xAC/f.aaf" })
    {
        const auto path = u8(text);
        CHECK(fileUrlToPath(pathToFileUrl(path)) == path);
    }
}
