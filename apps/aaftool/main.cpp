#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>
#include <aaf/core/document.hpp>
#include <aaf/core/writer.hpp>

#include "dump.hpp"

#include <charconv>

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
  dump <file> [--json] [--depth N] [--header|--metadict]
                                   print the object tree (default: from the root)
  validate <file> [--json]         check the file; exit 1 if there are errors
  roundtrip <in> <out> [--v3|--v4] [--regenerate-layout]
                                   load the AAF file and save it again unchanged
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

auto cmdDump(std::span<const std::string_view> args) -> int
{
    if (args.empty())
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    bool json = false;
    int depth = aaftool::kUnlimitedDepth;
    enum class Start {
        root,
        header,
        metadict,
    } start = Start::root;
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--json")
        {
            json = true;
        }
        else if (args[i] == "--header")
        {
            start = Start::header;
        }
        else if (args[i] == "--metadict")
        {
            start = Start::metadict;
        }
        else if (args[i] == "--depth" && i + 1 < args.size())
        {
            const auto text = args[++i];
            if (std::from_chars(text.data(), text.data() + text.size(), depth).ec != std::errc{} || depth < 0)
            {
                std::println(stderr, "aaftool: invalid depth '{}'", text);
                return 2;
            }
        }
        else
        {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
    }
    auto doc = aaf::Document::open(args[0]);
    if (!doc)
    {
        return reportError(doc.error());
    }
    aaf::ObjectId id = aaf::Document::root();
    if (start == Start::header)
    {
        id = doc->header();
    }
    else if (start == Start::metadict)
    {
        id = doc->metaDictionary();
    }
    if (id == aaf::kNoObject)
    {
        std::println(stderr, "aaftool: requested object is missing");
        return 1;
    }
    if (json)
    {
        std::println("{}", aaftool::toJson(*doc, id, depth).dump(1));
    }
    else
    {
        std::print("{}", aaftool::toText(*doc, id, depth));
    }
    return 0;
}

auto cmdValidate(std::span<const std::string_view> args) -> int
{
    if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "--json"))
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    auto doc = aaf::Document::open(args[0]);
    if (!doc)
    {
        return reportError(doc.error());
    }
    const auto diagnostics = aaf::validate(*doc);
    const auto errors = std::ranges::count(diagnostics, aaf::Diagnostic::Severity::error, &aaf::Diagnostic::severity);
    if (args.size() == 2)
    {
        auto list = nlohmann::ordered_json::array();
        for (const auto& d : diagnostics)
        {
            list.push_back({ { "severity", aaf::to_string(d.severity) }, { "object", d.object }, { "pid", d.pid }, { "message", d.message } });
        }
        std::println("{}", list.dump(1));
    }
    else
    {
        for (const auto& d : diagnostics)
        {
            std::println("{}: object {}: {}", aaf::to_string(d.severity), d.object, d.message);
        }
        const auto warnings = std::ranges::count(diagnostics, aaf::Diagnostic::Severity::warning, &aaf::Diagnostic::severity);
        std::println("{}: {} objects, {} errors, {} warnings, {} notes", args[0], doc->objectCount(), errors, warnings, static_cast<std::ptrdiff_t>(diagnostics.size()) - errors - warnings);
    }
    return errors > 0 ? 1 : 0;
}

auto cmdRoundtrip(std::span<const std::string_view> args) -> int
{
    if (args.size() < 2)
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    aaf::WriteOptions options;
    for (const auto arg : args.subspan(2))
    {
        if (arg == "--v3")
        {
            options.version = aaf::cfb::Version::v3;
        }
        else if (arg == "--v4")
        {
            options.version = aaf::cfb::Version::v4;
        }
        else if (arg == "--regenerate-layout")
        {
            options.preserveLayout = false;
        }
        else
        {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
    }
    auto doc = aaf::Document::open(args[0]);
    if (!doc)
    {
        return reportError(doc.error());
    }
    if (auto r = aaf::save(*doc, args[1], options); !r)
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
    if (args[0] == "dump")
    {
        return cmdDump(rest);
    }
    if (args[0] == "validate")
    {
        return cmdValidate(rest);
    }
    if (args[0] == "roundtrip")
    {
        return cmdRoundtrip(rest);
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
