#include <aaf/rpc/file_url.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace aaf::rpc
{

namespace
{

auto unreserved(unsigned char c) -> bool
{
    return std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~';
}

auto encode(std::string_view text, std::string_view keep) -> std::string
{
    constexpr std::array<char, 16> hex = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
    std::string out;
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (unreserved(c) || keep.contains(ch))
        {
            out += ch;
        }
        else
        {
            out += '%';
            out += hex.at(c >> 4U);
            out += hex.at(c & 0x0FU);
        }
    }
    return out;
}

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

auto decode(std::string_view text) -> std::optional<std::string>
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%')
        {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size())
        {
            return std::nullopt;
        }
        const auto high = hexValue(text[i + 1]);
        const auto low = hexValue(text[i + 2]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        out += static_cast<char>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low));
        i += 2;
    }
    return out;
}

auto driveLetter(std::string_view text) -> bool
{
    return text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 && text[1] == ':';
}

}

auto pathToFileUrl(const std::filesystem::path& path) -> std::string
{
    const auto generic = path.generic_u8string();
    const std::string text(generic.begin(), generic.end());
    if (text.starts_with("//"))
    {
        return "file:" + encode(text, "/");
    }
    if (driveLetter(text))
    {
        return "file:///" + text.substr(0, 2) + encode(text.substr(2), "/");
    }
    return "file://" + encode(text, "/");
}

auto fileUrlToPath(std::string_view url) -> std::optional<std::filesystem::path>
{
    std::string lower(url.substr(0, std::min<std::size_t>(url.size(), 7)));
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
    if (lower != "file://")
    {
        return std::nullopt;
    }
    auto rest = url.substr(7);
    if (const auto end = rest.find_first_of("?#"); end != std::string_view::npos)
    {
        rest = rest.substr(0, end);
    }
    const auto slash = rest.find('/');
    const auto host = rest.substr(0, slash == std::string_view::npos ? rest.size() : slash);
    const auto pathPart = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    const auto decoded = decode(pathPart);
    if (!decoded || decoded->empty())
    {
        return std::nullopt;
    }
    std::string text;
    if (!host.empty() && host != "localhost")
    {
        auto hostDecoded = decode(host);
        if (!hostDecoded)
        {
            return std::nullopt;
        }
        text = "//" + *hostDecoded + *decoded;
    }
    else if (decoded->size() >= 3 && driveLetter(std::string_view(*decoded).substr(1)))
    {
        text = decoded->substr(1);
    }
    else
    {
        text = *decoded;
    }
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}
