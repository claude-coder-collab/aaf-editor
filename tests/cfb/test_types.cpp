#include <aaf/cfb/types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace aaf::cfb;

TEST_CASE("CLSID formats and parses in registry form", "[cfb][types]")
{
    const auto parsed = parseClsid("{42464141-000d-4d4f-060e-2b34010101ff}");
    REQUIRE(parsed);
    CHECK(parsed->bytes[0] == std::byte{ 0x41 });
    CHECK(parsed->bytes[3] == std::byte{ 0x42 });
    CHECK(parsed->bytes[4] == std::byte{ 0x0d });
    CHECK(parsed->bytes[8] == std::byte{ 0x06 });
    CHECK(toString(*parsed) == "{42464141-000d-4d4f-060e-2b34010101ff}");
    CHECK(parseClsid("42464141-000d-4d4f-060e-2b34010101FF").value() == *parsed);
    CHECK_FALSE(parseClsid("{42464141-000d-4d4f-060e-2b34010101f}"));
    CHECK_FALSE(parseClsid("{4246414x-000d-4d4f-060e-2b34010101ff}"));
    CHECK(Clsid{}.isNull());
}

TEST_CASE("UTF-16 and UTF-8 conversion round-trips", "[cfb][types]")
{
    const std::u16string text = u"Header-2 é中\U0001F600";
    const auto utf8 = toUtf8(text);
    CHECK(utf8 == "Header-2 \xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80");
    CHECK(toUtf16(utf8).value() == text);
    CHECK(toUtf8(std::u16string(1, char16_t{ 0xD800 })) == "\xef\xbf\xbd");
    CHECK_FALSE(toUtf16("\xc0\x80"));
    CHECK_FALSE(toUtf16("\xe4\xb8"));
    CHECK_FALSE(toUtf16("\xed\xa0\x80"));
}

TEST_CASE("Names are ordered by length, then case-insensitively", "[cfb][types]")
{
    CHECK(compareNames(u"b", u"aa") < 0);
    CHECK(compareNames(u"abc", u"ABC") == 0);
    CHECK(compareNames(u"abc", u"abd") < 0);
    CHECK(compareNames(u"é", u"É") == 0);
    CHECK(compareNames(u"ā", u"Ā") == 0);
    CHECK(compareNames(u"я", u"Я") == 0);
    CHECK(upperCase(u'z') == u'Z');
    CHECK(upperCase(u'_') == u'_');
    CHECK(upperCase(u'ÿ') == u'Ÿ');
    CHECK(upperCase(u'ĺ') == u'Ĺ');
}

TEST_CASE("Entry names are validated", "[cfb][types]")
{
    CHECK(validateName(u"properties"));
    CHECK(validateName(std::u16string(31, u'x')));
    CHECK_FALSE(validateName(u""));
    CHECK_FALSE(validateName(std::u16string(32, u'x')));
    CHECK_FALSE(validateName(u"a/b"));
    CHECK_FALSE(validateName(u"a!b"));
}
