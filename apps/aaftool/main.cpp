#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>

#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view kUsage = R"(usage: aaftool <command> [args]

commands:
  cfb <file>                       list the compound file directory tree
  cfb-roundtrip <in> <out> [--v3|--v4]
                                   read and rewrite the compound file container
  --version                        print the version
)";

auto reportError(const aaf::Error& error) -> int
{
    std::println(stderr, "aaftool: {}", aaf::to_string(error));
    return 1;
}

void printTree(const aaf::cfb::Container& c, aaf::cfb::EntryId id, int depth)
{
    const auto& e = c.entry(id);
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    const std::string name = id == 0 ? std::string("/") : aaf::cfb::toUtf8(e.name);
    if (e.isStream())
    {
        std::println("{}{}  stream {}", indent, name, e.size);
        return;
    }
    std::println("{}{}  storage {}", indent, name, aaf::cfb::toString(e.clsid));
    for (const auto child : e.children)
    {
        printTree(c, child, depth + 1);
    }
}

auto cmdCfb(std::span<const std::string_view> args) -> int
{
    if (args.size() != 1)
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    auto c = aaf::cfb::Container::openFile(args[0]);
    if (!c)
    {
        return reportError(c.error());
    }
    const auto& h = c->header();
    std::println("{}: CFB v{}, {}-byte sectors, {} FAT, {} mini FAT, {} DIFAT sectors, header CLSID {}",
        args[0],
        static_cast<int>(h.version),
        h.sectorSize,
        h.fatSectors,
        h.miniFatSectors,
        h.difatSectors,
        aaf::cfb::toString(h.clsid));
    printTree(*c, 0, 0);
    return 0;
}

auto cmdCfbRoundtrip(std::span<const std::string_view> args) -> int
{
    if (args.size() < 2 || args.size() > 3)
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    auto c = aaf::cfb::Container::openFile(args[0]);
    if (!c)
    {
        return reportError(c.error());
    }
    auto builder = aaf::cfb::Builder::fromContainer(*c);
    if (!builder)
    {
        return reportError(builder.error());
    }
    if (args.size() == 3)
    {
        if (args[2] == "--v3")
        {
            builder->setVersion(aaf::cfb::Version::v3);
        }
        else if (args[2] == "--v4")
        {
            builder->setVersion(aaf::cfb::Version::v4);
        }
        else
        {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
    }
    if (auto r = aaf::cfb::writeFile(*builder, args[1]); !r)
    {
        return reportError(r.error());
    }
    return 0;
}

auto run(std::span<char*> argv) -> int
{
    const std::vector<std::string_view> args(argv.begin() + 1, argv.end());
    if (args.empty() || args[0] == "--help" || args[0] == "-h")
    {
        std::print("{}", kUsage);
        return args.empty() ? 2 : 0;
    }
    const auto rest = std::span(args).subspan(1);
    if (args[0] == "--version")
    {
        std::println("aaftool {}", AAF_VERSION);
        return 0;
    }
    if (args[0] == "cfb")
    {
        return cmdCfb(rest);
    }
    if (args[0] == "cfb-roundtrip")
    {
        return cmdCfbRoundtrip(rest);
    }
    std::println(stderr, "aaftool: unknown command '{}'", args[0]);
    std::print(stderr, "{}", kUsage);
    return 2;
}

}

auto main(int argc, char** argv) -> int
{
    try
    {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& e)
    {
        (void) std::fputs("aaftool: ", stderr);
        (void) std::fputs(e.what(), stderr);
        (void) std::fputs("\n", stderr);
        return 1;
    } catch (...)
    {
        (void) std::fputs("aaftool: unknown error\n", stderr);
        return 1;
    }
}
