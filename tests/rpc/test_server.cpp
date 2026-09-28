#include "fixtures.hpp"

#include <aaf/rpc/json_writer.hpp>
#include <aaf/rpc/server.hpp>
#include <aaf/timeline/timeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <format>
#include <functional>
#include <random>

using namespace aaf;
using namespace aaf::rpc;
using namespace aaf::test;

namespace
{

class Client
{
public:
    Client()
    {
        server_.setEventSink([this](const std::string& method, const Json& params) { events.emplace_back(method, params); });
    }

    auto request(const std::string& method, const Json& params = Json::object()) -> Json
    {
        const auto response = Json::parse(server_.handle(Json{ { "jsonrpc", "2.0" }, { "id", ++id_ }, { "method", method }, { "params", params } }.dump()));
        REQUIRE(response["id"] == id_);
        return response;
    }

    auto result(const std::string& method, const Json& params = Json::object()) -> Json
    {
        auto response = request(method, params);
        INFO(method << ": " << response.dump());
        REQUIRE(response.contains("result"));
        return response["result"];
    }

    auto raw(std::string_view text) -> Json { return Json::parse(server_.handle(text)); }

    std::vector<std::pair<std::string, Json>> events;

private:
    Server server_;
    int id_ = 0;
};

auto sample() -> std::string
{
    return (fixturesDir() / "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf").string();
}

auto findChild(Client& c, std::int64_t parent, std::string_view cls) -> Json
{
    const auto children = c.result("tree.children", { { "id", parent } });
    for (const auto& item : children["items"])
    {
        if (item["class"].get<std::string>() == cls)
        {
            return item;
        }
    }
    FAIL("no child of class " << cls);
    return {};
}

/// Expands a compact `timeline.get` result the way the UI does: shared sources, default labels, classes and hasLength.
auto expandTimeline(Json t) -> Json
{
    const auto sources = t["sources"];
    std::function<void(Json&)> expand = [&](Json& items) -> void {
        for (auto& item : items)
        {
            auto kind = item["kind"].get<std::string>();
            if (!item.contains("class"))
            {
                kind.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(kind.front())));
                item["class"] = kind;
            }
            if (!item.contains("hasLength"))
            {
                item["hasLength"] = true;
            }
            if (item.contains("source"))
            {
                auto source = sources.at(item["source"]["ref"].get<std::size_t>());
                source["slotId"] = item["source"]["slotId"];
                source["startTime"] = item["source"]["startTime"];
                item["source"] = source;
                if (!item.contains("label"))
                {
                    item["label"] = source["mobName"];
                }
            }
            if (item.contains("nested"))
            {
                for (auto& nested : item["nested"])
                {
                    expand(nested);
                }
            }
        }
    };
    for (auto& track : t["tracks"])
    {
        expand(track["items"]);
    }
    t.erase("sources");
    return t;
}

auto findProperty(const Json& object, std::string_view name) -> Json
{
    for (const auto& p : object["properties"])
    {
        if (p["name"].get<std::string>() == name)
        {
            return p;
        }
    }
    return nullptr;
}

}

TEST_CASE("Values round-trip through the tagged JSON form", "[rpc][json]")
{
    using namespace aaf::literals;
    const std::vector<Value> values = {
        Value(),
        Value(true),
        Value(std::int64_t{ -9'007'199'254'740'993 }),
        Value(std::uint64_t{ 18'446'744'073'709'551'615ULL }),
        Value(std::string("\xc3\xa9t\xc3\xa9")),
        Value("0d010101-0101-2f00-060e-2b3402060101"_auid),
        Value(MobId::generate()),
        Value(Value::Enum{ 2, "VersionDebug" }),
        Value(Value::ExtEnum{ "0d010102-0101-0100-060e-2b3404010101"_auid, "OperationCategory_Effect" }),
        Value(Value::Record{ { "Numerator", "Denominator" }, { Value(std::int64_t{ 30000 }), Value(std::int64_t{ 1001 }) } }),
        Value(Value::Array{ Value(std::string("a")), Value(std::string("b")) }),
        Value(Value::Opaque{ "0d010101-0101-2f00-060e-2b3402060101"_auid, { std::byte{ 1 }, std::byte{ 0xFF } } }),
        Value(Value::Bytes{ std::byte{ 0xAB } }),
    };
    for (const auto& v : values)
    {
        const auto json = toJson(v);
        INFO(json.dump());
        const auto back = valueFromJson(Json::parse(json.dump()));
        REQUIRE(back);
        CHECK(*back == v);
    }
    CHECK(toJson(Value(std::int64_t{ 5 }))["v"] == "5");
    CHECK_FALSE(valueFromJson(Json{ { "t", "int" }, { "v", "12x" } }));
    CHECK_FALSE(valueFromJson(Json{ { "t", "nope" } }));
    CHECK_FALSE(valueFromJson(Json(5)));
}

TEST_CASE("The server speaks JSON-RPC 2.0", "[rpc][server]")
{
    Client c;
    CHECK(c.raw("not json")["error"]["code"] == -32700);
    CHECK(c.raw(R"({"jsonrpc":"1.0","id":1,"method":"doc.info"})")["error"]["code"] == -32600);
    CHECK(c.request("no.such.method")["error"]["code"] == -32601);
    CHECK(c.request("doc.open")["error"]["code"] == -32602);
    CHECK(c.request("tree.children", { { "id", 0 } })["error"]["message"] == "no document is open");
    CHECK(c.result("doc.info")["open"] == false);
    Server server;
    CHECK(server.handle(R"({"jsonrpc":"2.0","method":"doc.info"})").empty());
}

TEST_CASE("Documents can be browsed through the RPC API", "[rpc][server]")
{
    Client c;
    const auto info = c.result("doc.open", { { "path", sample() } });
    CHECK(info["open"] == true);
    CHECK(info["dirty"] == false);
    CHECK(info["version"] == 3);
    REQUIRE_FALSE(c.events.empty());
    CHECK(c.events.back().first == "doc.opened");

    const auto root = c.result("tree.children", { { "id", 0 } });
    CHECK(root["total"] == 2);
    const auto header = findChild(c, 0, "Header");
    const auto content = findChild(c, header["id"], "ContentStorage");
    const auto page = c.result("tree.children", { { "id", content["id"] }, { "offset", 10 }, { "limit", 5 } });
    CHECK(page["items"].size() == 5);
    CHECK(page["total"].get<int>() > 100);
    CHECK_FALSE(page["items"][0]["key"].get<std::string>().empty());

    const auto mobId = page["items"][0]["id"];
    const auto mob = c.result("object.get", { { "id", mobId } });
    CHECK(mob["attached"] == true);
    const auto name = findProperty(mob, "MobID");
    REQUIRE(name.is_object());
    CHECK(name["value"]["t"] == "mobid");
    CHECK(mob["types"].contains(name["type"].get<std::string>()));
    CHECK(c.result("tree.path", { { "id", mobId } }) == Json::array({ 0, header["id"], content["id"], mobId }));

    const auto sequences = c.result("search.query", { { "class", "Sequence" }, { "limit", 3 } });
    CHECK(sequences.size() == 3);
    const auto sequence = c.result("object.get", { { "id", sequences[0]["id"] } });
    const auto dataDefinition = findProperty(sequence, "DataDefinition");
    CHECK(dataDefinition["kind"] == "weakRef");
    CHECK(dataDefinition["target"]["id"].is_number());
    CHECK_FALSE(c.result("object.candidates", { { "id", sequences[0]["id"] }, { "pid", dataDefinition["pid"] } }).empty());
    CHECK_FALSE(c.result("search.query", { { "text", "mob" }, { "limit", 5 } }).empty());
    CHECK(c.result("doc.validate").is_array());
    CHECK_FALSE(c.result("model.subclasses", { { "class", "Segment" } }).empty());
}

TEST_CASE("Edits through the RPC API are undoable and emit events", "[rpc][server]")
{
    Client c;
    c.result("doc.open", { { "path", sample() } });
    const auto mobs = c.result("search.query", { { "class", "CompositionMob" }, { "limit", 1 } });
    const auto mobId = mobs[0]["id"];
    const auto mob = c.result("object.get", { { "id", mobId } });
    const auto name = findProperty(mob, "Name");
    const auto pid = name["pid"];

    c.events.clear();
    const auto changes = c.result("object.setProperty", { { "id", mobId }, { "pid", pid }, { "value", { { "t", "string" }, { "v", "Renamed via RPC" } } } });
    CHECK(changes["objects"] == Json::array({ mobId }));
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].first == "doc.changed");
    CHECK(c.events[0].second["info"]["dirty"] == true);
    CHECK(c.events[0].second["info"]["undo"] == "Set Name");
    CHECK(findProperty(c.result("object.get", { { "id", mobId } }), "Name")["value"]["v"] == "Renamed via RPC");

    const auto rejected = c.request("object.setProperty", { { "id", mobId }, { "pid", pid }, { "value", { { "t", "int" }, { "v", "3" } } } });
    CHECK(rejected["error"]["code"] == -32000);

    c.result("edit.undo");
    CHECK(findProperty(c.result("object.get", { { "id", mobId } }), "Name")["value"]["v"] == name["value"]["v"]);
    c.result("edit.redo");
    CHECK(c.result("edit.history")["position"] == 1);

    const auto slots = findProperty(mob, "Slots");
    const auto created = c.result("object.create", { { "parent", mobId }, { "pid", slots["pid"] }, { "class", "TimelineMobSlot" }, { "index", 0 } });
    CHECK(c.result("object.get", { { "id", created["id"] } })["class"] == "TimelineMobSlot");
    CHECK(c.result("tree.children", { { "id", mobId } })["items"][0]["id"] == created["id"]);
    c.result("object.move", { { "parent", mobId }, { "pid", slots["pid"] }, { "from", 0 }, { "to", 1 } });
    c.result("object.delete", { { "id", created["id"] } });
    CHECK(c.result("object.get", { { "id", created["id"] } })["attached"] == false);
    CHECK(c.result("doc.info")["canUndo"] == true);

    const auto dir = std::filesystem::temp_directory_path() / std::format("aaf-rpc-{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    const auto saved = c.result("doc.saveAs", { { "path", (dir / "out.aaf").string() }, { "version", 4 } });
    CHECK(saved["dirty"] == false);
    CHECK(saved["name"] == "out.aaf");
    Client reopened;
    CHECK(reopened.result("doc.open", { { "path", (dir / "out.aaf").string() } })["version"] == 4);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Embedded essence can be extracted and replaced through the RPC API", "[rpc][server]")
{
    Client c;
    c.result("doc.open", { { "path", (fixturesDir() / "aafsdk/examples/com-api/ExportPCM/ExportPCM_NoCodecDef.aaf").string() } });
    const auto essence = c.result("search.query", { { "class", "EssenceData" }, { "limit", 1 } });
    const auto dir = std::filesystem::temp_directory_path() / std::format("aaf-rpc-essence-{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    const auto extracted = c.result("essence.extract", { { "id", essence[0]["id"] }, { "path", (dir / "e.bin").string() } });
    CHECK(extracted["size"].get<std::uint64_t>() == std::filesystem::file_size(dir / "e.bin"));
    const auto payload = patternBytes(5000, 3);
    REQUIRE(cfb::writeFileAtomic(dir / "new.bin", [&](cfb::ByteSink& sink) { return sink.write(payload); }));
    c.result("essence.replace", { { "id", essence[0]["id"] }, { "path", (dir / "new.bin").string() } });
    const auto data = findProperty(c.result("object.get", { { "id", essence[0]["id"] } }), "Data");
    CHECK(data["size"] == 5000);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Timelines are available through the RPC API", "[rpc][timeline]")
{
    Client c;
    c.result("doc.open", { { "path", sample() } });
    const auto mobs = c.result("timeline.mobs");
    REQUIRE(mobs.is_array());
    const auto composition = std::ranges::find_if(mobs, [](const Json& m) { return m["kind"] == "composition"; });
    REQUIRE(composition != mobs.end());
    const auto t = expandTimeline(c.result("timeline.get", { { "mob", (*composition)["id"] } }));
    REQUIRE_FALSE(t["tracks"].empty());
    Json clip = nullptr;
    for (const auto& track : t["tracks"])
    {
        CHECK(track["editRate"]["den"].get<int>() > 0);
        for (const auto& item : track["items"])
        {
            if (item["kind"] == "sourceClip" && item["source"]["mob"].is_number() && clip.is_null())
            {
                clip = item;
            }
        }
    }
    REQUIRE_FALSE(clip.is_null());
    const auto chain = c.result("timeline.resolve", { { "clip", clip["object"] } });
    CHECK(chain["status"] == "resolved");
    CHECK_FALSE(chain["links"].empty());
    CHECK(c.request("timeline.get", { { "mob", 0 } })["error"]["code"] == -32000);
}

TEST_CASE("Timeline operations are available through the RPC API", "[rpc][timeline]")
{
    Client c;
    c.result("doc.open", { { "path", sample() } });
    const auto mobs = c.result("timeline.mobs");
    Json track = nullptr;
    Json mob = nullptr;
    for (const auto& m : mobs)
    {
        if (m["kind"] != "composition" || !track.is_null())
        {
            continue;
        }
        const auto t = c.result("timeline.get", { { "mob", m["id"] } });
        for (const auto& candidate : t["tracks"])
        {
            const auto clips = std::ranges::count_if(candidate["items"], [](const Json& i) { return i["kind"] == "sourceClip"; });
            if (clips >= 2 && candidate["effects"].empty() && track.is_null())
            {
                track = candidate;
                mob = m;
            }
        }
    }
    REQUIRE_FALSE(track.is_null());
    const auto first = *std::ranges::find_if(track["items"], [](const Json& i) { return i["kind"] == "sourceClip" && i["length"].get<std::int64_t>() > 2; });
    const auto split = c.result("timeline.op", { { "op", "split" }, { "slot", track["slot"] }, { "position", first["start"].get<std::int64_t>() + first["length"].get<std::int64_t>() / 2 } });
    CHECK(split["id"].is_number());
    CHECK_FALSE(split["changes"]["objects"].empty());
    const auto full = expandTimeline(c.result("timeline.get", { { "mob", mob["id"] } }));
    CHECK_FALSE(full["partial"].get<bool>());
    CHECK(full["slots"].size() == full["tracks"].size());
    const auto partial = expandTimeline(c.result("timeline.get", { { "mob", mob["id"] }, { "changed", split["changes"]["objects"] } }));
    CHECK(partial["partial"].get<bool>());
    CHECK(partial["slots"] == full["slots"]);
    REQUIRE(partial["tracks"].size() == 1);
    CHECK(partial["tracks"][0] == *std::ranges::find_if(full["tracks"], [&](const Json& t) -> bool { return t["slot"] == track["slot"]; }));
    const auto outside = c.result("timeline.get", { { "mob", mob["id"] }, { "changed", Json::array({ 0 }) } });
    CHECK_FALSE(outside["partial"].get<bool>());
    CHECK(c.request("timeline.get", { { "mob", mob["id"] }, { "changed", "all" } })["error"]["code"] == -32000);
    c.result("timeline.op", { { "op", "lift" }, { "item", split["id"] } });
    c.result("timeline.op", { { "op", "trim" }, { "item", first["object"] }, { "edge", "tail" }, { "delta", -1 }, { "ripple", true } });
    const auto added = c.result("timeline.op", { { "op", "addTrack" }, { "mob", mob["id"] }, { "kind", "sound" }, { "name", "Music" } });
    CHECK(added["id"].is_number());
    const auto marker = c.result("timeline.op", { { "op", "addMarker" }, { "mob", mob["id"] }, { "position", 5 }, { "comment", "Check this" } });
    CHECK(marker["id"].is_number());
    const auto relinked = c.result("timeline.op", { { "op", "relink" }, { "find", "/" }, { "replace", "\\" } });
    CHECK(relinked["count"].get<int>() > 0);
    CHECK(c.request("timeline.op", { { "op", "explode" } })["error"]["code"] == -32000);
    CHECK(c.request("timeline.op", { { "op", "trim" }, { "item", first["object"] }, { "edge", "middle" }, { "delta", 1 } })["error"]["code"] == -32000);
    CHECK(c.result("edit.history")["items"].size() == 6);
    CHECK(c.result("doc.validate").is_array());
}

TEST_CASE("The compact timeline form carries every projected field", "[rpc][timeline]")
{
    Client c;
    c.result("doc.open", { { "path", sample() } });
    auto document = Document::open(sample());
    REQUIRE(document);
    const timeline::Projector projector(*document);
    std::size_t compared = 0;
    std::function<void(const std::vector<timeline::Item>&, const Json&)> compare = [&](const std::vector<timeline::Item>& items, const Json& json) -> void {
        REQUIRE(json.size() == items.size());
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            const auto& j = json[i];
            CHECK(j["object"] == item.object);
            CHECK(j["kind"] == std::string(timeline::to_string(item.kind)));
            CHECK(j["class"] == item.className);
            CHECK(j["start"] == item.start);
            CHECK(j["length"] == item.length);
            CHECK(j["hasLength"] == item.hasLength);
            CHECK(j["label"] == item.label);
            CHECK(j.value("effect", "") == item.effect);
            CHECK(j.value("comment", "") == item.comment);
            CHECK(j.contains("timecode") == item.timecode.has_value());
            CHECK(j.contains("source") == item.source.has_value());
            if (item.source)
            {
                const auto& s = j["source"];
                CHECK(s["mobId"] == item.source->mobId.toString());
                CHECK(s["mob"] == (item.source->mob ? Json(*item.source->mob) : Json(nullptr)));
                CHECK(s["mobName"] == item.source->mobName);
                CHECK(s["mobKind"] == std::string(timeline::to_string(item.source->mobKind)));
                CHECK(s["original"] == item.source->original);
                CHECK(s["slotId"] == item.source->slotId);
                CHECK(s["startTime"] == item.source->startTime);
            }
            REQUIRE(j.value("nested", Json::array()).size() == item.nested.size());
            for (std::size_t n = 0; n < item.nested.size(); ++n)
            {
                compare(item.nested[n], j["nested"][n]);
            }
            ++compared;
        }
    };
    for (const auto& mob : projector.mobs())
    {
        const auto expected = projector.project(mob.object).value();
        const auto json = expandTimeline(c.result("timeline.get", { { "mob", mob.object } }));
        CHECK(json["name"] == expected.name);
        REQUIRE(json["tracks"].size() == expected.tracks.size());
        for (std::size_t t = 0; t < expected.tracks.size(); ++t)
        {
            CHECK(json["tracks"][t]["slot"] == expected.tracks[t].slot);
            compare(expected.tracks[t].items, json["tracks"][t]["items"]);
        }
    }
    CHECK(compared > 500);
}

TEST_CASE("JsonWriter produces valid compact JSON", "[rpc][json]")
{
    JsonWriter w;
    w.beginObject().key("a").value(std::int64_t{ -5 }).key("s").value("q\"\\\n\x01é").key("list").beginArray().value(true).null().beginObject().endObject().beginArray().endArray().value(std::uint64_t{ 18446744073709551615ULL }).endArray().key("raw").raw(R"({"x":1})").endObject();
    const auto parsed = Json::parse(w.str());
    CHECK(parsed["a"] == -5);
    CHECK(parsed["s"] == "q\"\\\n\x01é");
    CHECK(parsed["list"].size() == 5);
    CHECK(parsed["list"][4].get<std::uint64_t>() == 18446744073709551615ULL);
    CHECK(parsed["raw"]["x"] == 1);
    CHECK(w.str().find(' ') == std::string::npos);
}
