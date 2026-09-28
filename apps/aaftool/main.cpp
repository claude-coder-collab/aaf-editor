#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>
#include <aaf/core/document.hpp>
#include <aaf/core/writer.hpp>
#include <aaf/edit/operations.hpp>
#include <aaf/edit/session.hpp>
#include <aaf/rpc/server.hpp>
#include <aaf/timeline/timeline.hpp>

#include "dump.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <map>
#include <memory>

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
  timeline <file> [--mobs] [--mob NAME|ID] [--json]
                                   list mobs, or show a mob's tracks (default: first top-level composition)
  serve                            answer JSON-RPC requests from stdin, one per line; each output line is
                                   {"response": ..., "events": [...]} (used by the UI end-to-end tests)
  rpc <file> (--call METHOD PARAMS-JSON)... [--save OUT]
                                   run editor RPC calls (see SPEC §8.3) and optionally save the result
  extract <file> --list            list embedded essence
  extract <file> <mobid|index> <out>
                                   write an embedded essence stream to a file
  set-essence <in> <mobid|index> <data> <out>
                                   replace an embedded essence stream with a file's contents
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

struct Essence
{
    aaf::ObjectId object = aaf::kNoObject;
    std::string mobId;
    std::string mobName;
    const aaf::StreamProperty* stream = nullptr;
};

auto listEssence(const aaf::Document& doc) -> std::vector<Essence>
{
    const auto& model = doc.model();
    const auto* essenceClass = model.findClassByName("EssenceData");
    const auto* mobClass = model.findClassByName("Mob");
    const auto* dataDef = model.findProperty("EssenceData", "Data");
    if (essenceClass == nullptr || mobClass == nullptr || dataDef == nullptr)
    {
        return {};
    }
    std::map<std::string, std::string> names;
    std::vector<Essence> out;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        const auto& o = doc.object(i);
        if (!doc.isAttached(i))
        {
            continue;
        }
        if (model.isA(o.classId, mobClass->id))
        {
            const auto id = doc.value(i, "Mob", "MobID");
            const auto name = doc.value(i, "Mob", "Name");
            if (id)
            {
                names[id->toString()] = name && name->is<std::string>() ? name->as<std::string>() : std::string{};
            }
        }
        else if (model.isA(o.classId, essenceClass->id))
        {
            const auto* p = o.find(dataDef->pid);
            const auto id = doc.value(i, "EssenceData", "MobID");
            out.push_back({ i, id ? id->toString() : std::string{}, {}, p == nullptr ? nullptr : std::get_if<aaf::StreamProperty>(&p->payload) });
        }
    }
    for (auto& e : out)
    {
        e.mobName = names[e.mobId];
    }
    return out;
}

auto findEssence(const std::vector<Essence>& all, std::string_view key) -> const Essence*
{
    std::size_t index = 0;
    if (std::from_chars(key.data(), key.data() + key.size(), index).ec == std::errc{} && index < all.size() && std::to_string(index) == key)
    {
        return &all[index];
    }
    const auto it = std::ranges::find(all, key, &Essence::mobId);
    return it == all.end() ? nullptr : &*it;
}

auto cmdExtract(std::span<const std::string_view> args) -> int
{
    if (args.size() != 2 && args.size() != 3)
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    auto doc = aaf::Document::open(args[0]);
    if (!doc)
    {
        return reportError(doc.error());
    }
    const auto all = listEssence(*doc);
    if (args.size() == 2 && args[1] == "--list")
    {
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            std::println("{}\t{}\t{}\t{}", i, all[i].mobId, all[i].stream != nullptr ? all[i].stream->size : 0, all[i].mobName);
        }
        return 0;
    }
    if (args.size() != 3)
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    const auto* essence = findEssence(all, args[1]);
    if (essence == nullptr || essence->stream == nullptr)
    {
        std::println(stderr, "aaftool: no embedded essence '{}'", args[1]);
        return 1;
    }
    auto written = aaf::cfb::writeFileAtomic(args[2], [&](aaf::cfb::ByteSink& sink) -> aaf::Result<void> {
        auto copied = aaf::copyStream(*doc, *essence->stream, sink);
        if (!copied)
        {
            return std::unexpected(copied.error());
        }
        return {};
    });
    if (!written)
    {
        return reportError(written.error());
    }
    return 0;
}

auto cmdSetEssence(std::span<const std::string_view> args) -> int
{
    if (args.size() != 4)
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    auto session = aaf::edit::Session::open(args[0]);
    if (!session)
    {
        return reportError(session.error());
    }
    const auto all = listEssence(session->document());
    const auto* essence = findEssence(all, args[1]);
    if (essence == nullptr)
    {
        std::println(stderr, "aaftool: no embedded essence '{}'", args[1]);
        return 1;
    }
    auto source = aaf::cfb::FileSource::open(args[2]);
    if (!source)
    {
        return reportError(source.error());
    }
    std::shared_ptr<const aaf::cfb::ByteSource> shared = std::move(*source);
    const auto pid = session->document().model().findProperty("EssenceData", "Data")->pid;
    const auto object = essence->object;
    auto changed = session->execute("Replace essence", [&](aaf::edit::Transaction& tx) -> aaf::Result<void> { return aaf::edit::setStreamData(tx, object, pid, shared); });
    if (!changed)
    {
        return reportError(changed.error());
    }
    if (auto r = session->save(args[3]); !r)
    {
        return reportError(r.error());
    }
    return 0;
}

auto itemJson(const aaf::timeline::Item& item) -> nlohmann::ordered_json
{
    nlohmann::ordered_json j = { { "object", item.object }, { "kind", aaf::timeline::to_string(item.kind) }, { "class", item.className }, { "start", item.start }, { "length", item.length }, { "label", item.label } };
    if (!item.effect.empty())
    {
        j["effect"] = item.effect;
    }
    if (item.source)
    {
        j["source"] = { { "mobId", item.source->mobId.toString() }, { "slotId", item.source->slotId }, { "startTime", item.source->startTime }, { "found", item.source->mob.has_value() }, { "mobKind", aaf::timeline::to_string(item.source->mobKind) }, { "original", item.source->original } };
    }
    if (item.timecode)
    {
        j["timecode"] = { { "start", item.timecode->start }, { "fps", item.timecode->fps }, { "drop", item.timecode->drop } };
    }
    if (!item.nested.empty())
    {
        j["nested"] = nlohmann::ordered_json::array();
        for (const auto& track : item.nested)
        {
            auto list = nlohmann::ordered_json::array();
            for (const auto& child : track)
            {
                list.push_back(itemJson(child));
            }
            j["nested"].push_back(std::move(list));
        }
    }
    return j;
}

auto cmdTimeline(std::span<const std::string_view> args) -> int
{
    if (args.empty())
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    bool json = false;
    bool listMobs = false;
    std::string_view which;
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--json")
        {
            json = true;
        }
        else if (args[i] == "--mobs")
        {
            listMobs = true;
        }
        else if (args[i] == "--mob" && i + 1 < args.size())
        {
            which = args[++i];
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
    const aaf::timeline::Projector projector(*doc);
    const auto mobs = projector.mobs();
    if (listMobs)
    {
        for (const auto& m : mobs)
        {
            std::println("{}\t{}\t{}\t{} tracks\t{}{}", m.object, aaf::timeline::to_string(m.kind), m.mobId.toString(), m.tracks, m.name, m.topLevel ? " (top level)" : "");
        }
        return 0;
    }
    const aaf::timeline::MobSummary* chosen = nullptr;
    for (const auto& m : mobs)
    {
        if (which.empty() ? m.kind == aaf::timeline::MobKind::composition : (m.name == which || std::to_string(m.object) == which || m.mobId.toString() == which))
        {
            chosen = &m;
            break;
        }
    }
    if (chosen == nullptr)
    {
        std::println(stderr, "aaftool: no matching mob");
        return 1;
    }
    auto timeline = projector.project(chosen->object);
    if (!timeline)
    {
        return reportError(timeline.error());
    }
    if (json)
    {
        nlohmann::ordered_json tracks = nlohmann::ordered_json::array();
        for (const auto& track : timeline->tracks)
        {
            auto items = nlohmann::ordered_json::array();
            for (const auto& item : track.items)
            {
                items.push_back(itemJson(item));
            }
            auto effects = nlohmann::ordered_json::array();
            for (const auto& effect : track.effects)
            {
                effects.push_back({ { "object", effect.object }, { "name", effect.name } });
            }
            tracks.push_back({ { "slot", track.slot }, { "slotId", track.slotId }, { "name", track.name }, { "kind", aaf::timeline::to_string(track.kind) }, { "slotKind", aaf::timeline::to_string(track.slotKind) }, { "editRate", track.editRate.toString() }, { "origin", track.origin }, { "length", track.length }, { "effects", std::move(effects) }, { "items", std::move(items) } });
        }
        std::println("{}", nlohmann::ordered_json{ { "mob", timeline->mob }, { "name", timeline->name }, { "kind", aaf::timeline::to_string(timeline->kind) }, { "tracks", std::move(tracks) }, { "warnings", timeline->warnings } }.dump(1));
        return 0;
    }
    std::println("{} ({}, {})", timeline->name, aaf::timeline::to_string(timeline->kind), timeline->mobId.toString());
    for (const auto& track : timeline->tracks)
    {
        std::println("  slot {} {} [{}] rate {} length {}{}", track.slotId, aaf::timeline::to_string(track.kind), track.name, track.editRate.toString(), track.length, track.slotKind == aaf::timeline::SlotKind::event ? " (events)" : "");
        for (const auto& item : track.items)
        {
            std::println("    {:>8} +{:<6} {:<14} {}", item.start, item.length, aaf::timeline::to_string(item.kind), item.label);
        }
    }
    return 0;
}

auto cmdServe(std::span<const std::string_view> args) -> int
{
    if (!args.empty())
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    aaf::rpc::Server server;
    auto events = nlohmann::json::array();
    server.setEventSink([&events](const std::string& method, const nlohmann::json& params) -> void { events.push_back({ { "method", method }, { "params", params } }); });
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
        {
            continue;
        }
        events = nlohmann::json::array();
        const auto response = server.handle(line);
        const nlohmann::json out = { { "response", response.empty() ? nlohmann::json(nullptr) : nlohmann::json::parse(response) }, { "events", events } };
        std::cout << out.dump() << '\n'
                  << std::flush;
    }
    return 0;
}

auto cmdRpc(std::span<const std::string_view> args) -> int
{
    if (args.empty())
    {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    aaf::rpc::Server server;
    if (auto opened = server.call("doc.open", { { "path", std::string(args[0]) } }); !opened)
    {
        return reportError(opened.error());
    }
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--call" && i + 2 < args.size())
        {
            const auto method = std::string(args[i + 1]);
            nlohmann::json params;
            try
            {
                params = nlohmann::json::parse(args[i + 2]);
            } catch (const nlohmann::json::exception& e)
            {
                std::println(stderr, "aaftool: invalid JSON for {}: {}", method, e.what());
                return 2;
            }
            auto result = server.call(method, params);
            if (!result)
            {
                return reportError(result.error());
            }
            std::println("{}", result->dump());
            i += 2;
        }
        else if (args[i] == "--save" && i + 1 < args.size())
        {
            if (auto saved = server.call("doc.saveAs", { { "path", std::string(args[++i]) } }); !saved)
            {
                return reportError(saved.error());
            }
        }
        else
        {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
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
    if (args[0] == "serve")
    {
        return cmdServe(rest);
    }
    if (args[0] == "rpc")
    {
        return cmdRpc(rest);
    }
    if (args[0] == "timeline")
    {
        return cmdTimeline(rest);
    }
    if (args[0] == "extract")
    {
        return cmdExtract(rest);
    }
    if (args[0] == "set-essence")
    {
        return cmdSetEssence(rest);
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
