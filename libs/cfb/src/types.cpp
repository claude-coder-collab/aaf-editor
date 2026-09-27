#include <aaf/cfb/types.hpp>

#include <algorithm>
#include <format>

namespace aaf::cfb
{

namespace
{

constexpr std::array<int, 16> kClsidDisplayOrder = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };

auto hexValue(char c) -> int
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}

auto toString(const Clsid& clsid) -> std::string
{
    std::string out = "{";
    for (std::size_t i = 0; i < kClsidDisplayOrder.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            out.push_back('-');
        }
        out += std::format("{:02x}", std::to_integer<unsigned>(clsid.bytes[static_cast<std::size_t>(kClsidDisplayOrder[i])]));
    }
    out.push_back('}');
    return out;
}

auto parseClsid(std::string_view text) -> Result<Clsid>
{
    if (text.size() >= 2 && text.front() == '{' && text.back() == '}')
    {
        text = text.substr(1, text.size() - 2);
    }
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    {
        return fail(Errc::invalid_argument, std::format("malformed CLSID '{}'", text));
    }
    Clsid clsid;
    std::size_t pos = 0;
    for (const int index : kClsidDisplayOrder)
    {
        if (text[pos] == '-')
        {
            ++pos;
        }
        const int hi = hexValue(text[pos]);
        const int lo = hexValue(text[pos + 1]);
        if (hi < 0 || lo < 0)
        {
            return fail(Errc::invalid_argument, std::format("malformed CLSID '{}'", text));
        }
        clsid.bytes[static_cast<std::size_t>(index)] = static_cast<std::byte>(hi * 16 + lo);
        pos += 2;
    }
    return clsid;
}

auto toUtf8(std::u16string_view text) -> std::string
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
        {
            appendUtf8(out, 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(text[i + 1]) - 0xDC00));
            ++i;
        }
        else if (c >= 0xD800 && c <= 0xDFFF)
        {
            appendUtf8(out, 0xFFFD);
        }
        else
        {
            appendUtf8(out, c);
        }
    }
    return out;
}

auto toUtf16(std::string_view text) -> Result<std::u16string>
{
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        char32_t cp = 0;
        if (lead < 0x80)
        {
            len = 1;
            cp = lead;
        }
        else if ((lead & 0xE0) == 0xC0)
        {
            len = 2;
            cp = lead & 0x1FU;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            len = 3;
            cp = lead & 0x0FU;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            len = 4;
            cp = lead & 0x07U;
        }
        else
        {
            return fail(Errc::invalid_argument, "malformed UTF-8");
        }
        if (i + len > text.size())
        {
            return fail(Errc::invalid_argument, "truncated UTF-8");
        }
        for (std::size_t k = 1; k < len; ++k)
        {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80)
            {
                return fail(Errc::invalid_argument, "malformed UTF-8");
            }
            cp = (cp << 6) | (cont & 0x3FU);
        }
        constexpr std::array<char32_t, 5> kMinForLength = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < kMinForLength[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            return fail(Errc::invalid_argument, "invalid UTF-8 code point");
        }
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
        else
        {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}

auto upperCase(char16_t c) noexcept -> char16_t
{
    const auto u = static_cast<std::uint32_t>(c);
    auto shifted = [](std::uint32_t v, std::uint32_t delta) -> char16_t { return static_cast<char16_t>(v - delta); };
    if (u >= 'a' && u <= 'z')
    {
        return shifted(u, 0x20);
    }
    if ((u >= 0xE0 && u <= 0xFE && u != 0xF7))
    {
        return shifted(u, 0x20);
    }
    if (u == 0xFF)
    {
        return u'Ÿ';
    }
    if (u >= 0x100 && u <= 0x17F && u != 0x130 && u != 0x131 && u != 0x138 && u != 0x149 && u != 0x17F)
    {
        const bool oddBased = (u >= 0x139 && u <= 0x148) || (u >= 0x179 && u <= 0x17E);
        const bool lower = oddBased ? (u % 2 == 0) : (u % 2 == 1);
        return lower ? shifted(u, 1) : c;
    }
    if (u >= 0x3B1 && u <= 0x3C9 && u != 0x3C2)
    {
        return shifted(u, 0x20);
    }
    if (u >= 0x430 && u <= 0x44F)
    {
        return shifted(u, 0x20);
    }
    if (u >= 0x450 && u <= 0x45F)
    {
        return shifted(u, 0x50);
    }
    return c;
}

auto compareNames(std::u16string_view a, std::u16string_view b) noexcept -> std::strong_ordering
{
    if (a.size() != b.size())
    {
        return a.size() <=> b.size();
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const auto ua = upperCase(a[i]);
        const auto ub = upperCase(b[i]);
        if (ua != ub)
        {
            return ua <=> ub;
        }
    }
    return std::strong_ordering::equal;
}

auto validateName(std::u16string_view name) -> Result<void>
{
    if (name.empty())
    {
        return fail(Errc::invalid_argument, "entry name is empty");
    }
    if (name.size() > kMaxNameLength)
    {
        return fail(Errc::invalid_argument, std::format("entry name '{}' exceeds {} characters", toUtf8(name), kMaxNameLength));
    }
    if (std::ranges::any_of(name, [](char16_t c) -> bool { return c == u'/' || c == u'\\' || c == u':' || c == u'!' || c == 0; }))
    {
        return fail(Errc::invalid_argument, std::format("entry name '{}' contains an illegal character", toUtf8(name)));
    }
    return {};
}

}
