#include "fixtures.hpp"

#include <aaf/core/writer.hpp>
#include <aaf/edit/operations.hpp>
#include <aaf/edit/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <random>

using namespace aaf;
using namespace aaf::edit;
using namespace aaf::test;
using namespace aaf::literals;

namespace
{

constexpr std::string_view kSample = "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf";

auto openSession(std::string_view relative = kSample) -> Result<Session>
{
    return Session::open(fixturesDir() / relative);
}

auto allOfClass(const Document& doc, std::string_view name) -> std::vector<ObjectId>
{
    std::vector<ObjectId> out;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        const auto* cls = doc.classOf(i);
        if (cls != nullptr && cls->name == name && doc.isAttached(i))
        {
            out.push_back(i);
        }
    }
    return out;
}

auto firstOfClass(const Document& doc, std::string_view name) -> ObjectId
{
    const auto all = allOfClass(doc, name);
    REQUIRE_FALSE(all.empty());
    return all.front();
}

auto pid(const Document& doc, ObjectId id, std::string_view name) -> std::uint16_t
{
    auto p = pidOf(doc, id, name);
    REQUIRE(p);
    return *p;
}

auto stringValue(const Document& doc, ObjectId id, std::string_view name) -> std::string
{
    const auto* p = doc.object(id).find(pid(doc, id, name));
    if (p == nullptr)
    {
        return {};
    }
    return doc.decode(doc.object(id), *p).value().as<std::string>();
}

struct Snapshot
{
    std::vector<Object> objects;
    std::vector<std::vector<std::uint16_t>> referenced;
};

auto snapshot(const Document& doc) -> Snapshot
{
    Snapshot s;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        s.objects.push_back(doc.object(i));
    }
    s.referenced = doc.referencedProperties();
    return s;
}

/// Objects created after the snapshot must be detached; all others must equal the snapshot.
auto sameAs(const Document& doc, const Snapshot& s) -> std::string
{
    if (doc.referencedProperties() != s.referenced)
    {
        return "referenced properties differ";
    }
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        if (i < s.objects.size() ? !(doc.object(i) == s.objects[i]) : doc.isAttached(i))
        {
            return std::format("object {} differs", i);
        }
    }
    return {};
}

auto errorCount(const Document& doc) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count(validate(doc), Diagnostic::Severity::error, &Diagnostic::severity));
}

auto saveToMemory(const Document& doc) -> std::vector<std::byte>
{
    cfb::MemorySink sink;
    REQUIRE(write(doc, sink));
    return sink.take();
}

auto newMobId(std::uint8_t seed) -> MobId
{
    MobId id;
    const std::array<std::uint8_t, 12> label = { 0x06, 0x0a, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x0f, 0x20 };
    for (std::size_t i = 0; i < label.size(); ++i)
    {
        id.bytes[i] = std::byte{ label[i] };
    }
    id.bytes[12] = std::byte{ 0x13 };
    for (std::size_t i = 16; i < 32; ++i)
    {
        id.bytes[i] = static_cast<std::byte>(seed + i);
    }
    return id;
}

auto timestamp() -> Value
{
    Value::Record date{ { "year", "month", "day" }, { Value(std::int64_t{ 2026 }), Value(std::uint64_t{ 9 }), Value(std::uint64_t{ 27 }) } };
    Value::Record time{ { "hour", "minute", "second", "fraction" }, { Value(std::uint64_t{ 12 }), Value(std::uint64_t{ 0 }), Value(std::uint64_t{ 0 }), Value(std::uint64_t{ 0 }) } };
    return Value(Value::Record{ { "date", "time" }, { Value(std::move(date)), Value(std::move(time)) } });
}

}

TEST_CASE("Setting a property is undoable and marks the session dirty", "[edit][session]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto mob = firstOfClass(doc, "CompositionMob");
    const auto original = stringValue(doc, mob, "Name");
    const auto before = snapshot(doc);

    std::vector<ChangeSet> notifications;
    session.setListener([&](const ChangeSet& c) { notifications.push_back(c); });
    const auto changes = session.execute("Rename mob", [&](Transaction& tx) { return setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::string("Renamed"))); });
    REQUIRE(changes);
    CHECK(changes->objects == std::vector<ObjectId>{ mob });
    CHECK(changes->properties == std::vector<PropertyChange>{ { mob, pid(doc, mob, "Name") } });
    CHECK(notifications.size() == 1);
    CHECK(stringValue(doc, mob, "Name") == "Renamed");
    CHECK(session.dirty());
    CHECK(session.history() == std::vector<std::string>{ "Rename mob" });

    REQUIRE(session.undo());
    CHECK(stringValue(doc, mob, "Name") == original);
    CHECK(sameAs(doc, before).empty());
    CHECK_FALSE(session.dirty());
    CHECK_FALSE(session.undo());

    REQUIRE(session.redo());
    CHECK(stringValue(doc, mob, "Name") == "Renamed");
    CHECK_FALSE(session.redo());
    CHECK(notifications.size() == 3);
}

TEST_CASE("Invalid commands are rejected and leave the document unchanged", "[edit][session]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto before = snapshot(doc);
    const auto mob = firstOfClass(doc, "CompositionMob");
    const auto clip = firstOfClass(doc, "SourceClip");

    auto rejects = [&](std::string_view what, const Session::Command& command) {
        INFO(what);
        CHECK_FALSE(session.execute(std::string(what), command));
        CHECK(sameAs(doc, before).empty());
        CHECK(session.position() == 0);
    };
    rejects("wrong value type", [&](Transaction& tx) { return setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::int64_t{ 5 })); });
    rejects("property of another class", [&](Transaction& tx) { return setProperty(tx, mob, pid(doc, clip, "SourceMobSlotID"), Value(std::uint64_t{ 1 })); });
    rejects("required property", [&](Transaction& tx) { return removeProperty(tx, mob, pid(doc, mob, "MobID")); });
    rejects("abstract class", [&](Transaction& tx) { return createObject(tx, doc.model().findClassByName("Mob")->id).transform([](ObjectId) {}); });
    rejects("attached child", [&](Transaction& tx) { return insertIntoCollection(tx, mob, pid(doc, mob, "Slots"), 0, clip); });
    const auto cycle = session.execute("cycle", [&](Transaction& tx) -> Result<void> {
        const auto sequenceClass = doc.model().findClassByName("Sequence")->id;
        const auto a = createObject(tx, sequenceClass).value();
        const auto b = createObject(tx, sequenceClass).value();
        const auto components = pid(doc, a, "Components");
        if (auto r = insertIntoCollection(tx, a, components, 0, b); !r)
        {
            return r;
        }
        return insertIntoCollection(tx, b, components, 0, a);
    });
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().message.find("cycle") != std::string::npos);
    CHECK(sameAs(doc, before).empty());
    rejects("valid step followed by failure", [&](Transaction& tx) -> Result<void> {
        if (auto r = setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::string("x"))); !r)
        {
            return r;
        }
        return fail(Errc::invalid_argument, "stop");
    });
    rejects("result fails validation", [&](Transaction& tx) -> Result<void> {
        auto created = createObject(tx, doc.model().findClassByName("CompositionMob")->id);
        if (!created)
        {
            return std::unexpected(created.error());
        }
        const auto content = doc.object(mob).parent;
        if (auto r = setProperty(tx, *created, pid(doc, mob, "MobID"), Value(newMobId(1))); !r)
        {
            return r;
        }
        return insertIntoCollection(tx, content, pid(doc, content, "Mobs"), 0, *created);
    });
}

TEST_CASE("New objects can be built and inserted", "[edit][session]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto before = snapshot(doc);
    const auto content = doc.object(firstOfClass(doc, "CompositionMob")).parent;
    const auto dataDefinition = firstOfClass(doc, "DataDefinition");
    ObjectId mob = kNoObject;

    const auto result = session.execute("Add composition", [&](Transaction& tx) -> Result<void> {
        const auto& model = doc.model();
        auto check = [](const Result<void>& r) { return r; };
        mob = createObject(tx, model.findClassByName("CompositionMob")->id).value();
        const auto slot = createObject(tx, model.findClassByName("TimelineMobSlot")->id).value();
        const auto sequence = createObject(tx, model.findClassByName("Sequence")->id).value();
        const auto filler = createObject(tx, model.findClassByName("Filler")->id).value();
        for (const auto& r : {
                 setProperty(tx, mob, pid(doc, mob, "MobID"), Value(newMobId(7))),
                 setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::string("New composition"))),
                 setProperty(tx, mob, pid(doc, mob, "LastModified"), timestamp()),
                 setProperty(tx, mob, pid(doc, mob, "CreationTime"), timestamp()),
                 setProperty(tx, slot, pid(doc, slot, "SlotID"), Value(std::uint64_t{ 1 })),
                 setProperty(tx, slot, pid(doc, slot, "Origin"), Value(std::int64_t{ 0 })),
                 setProperty(tx, slot, pid(doc, slot, "EditRate"), Value(Value::Record{ { "Numerator", "Denominator" }, { Value(std::int64_t{ 25 }), Value(std::int64_t{ 1 }) } })),
                 setProperty(tx, filler, pid(doc, filler, "Length"), Value(std::int64_t{ 100 })),
                 setWeakRef(tx, filler, pid(doc, filler, "DataDefinition"), dataDefinition),
                 setWeakRef(tx, sequence, pid(doc, sequence, "DataDefinition"), dataDefinition),
                 ensureCollection(tx, sequence, pid(doc, sequence, "Components")),
                 insertIntoCollection(tx, sequence, pid(doc, sequence, "Components"), 0, filler),
                 setStrongRef(tx, slot, pid(doc, slot, "Segment"), sequence),
                 insertIntoCollection(tx, mob, pid(doc, mob, "Slots"), 0, slot),
                 insertIntoCollection(tx, content, pid(doc, content, "Mobs"), 0, mob),
             })
        {
            if (!check(r))
            {
                return r;
            }
        }
        return {};
    });
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result);
    CHECK(result->created.size() == 4);
    CHECK(doc.isAttached(mob));
    CHECK(errorCount(doc) == 0);

    auto reloaded = openMemory(saveToMemory(doc));
    REQUIRE(reloaded);
    auto reloadedDoc = Document::load(std::move(*reloaded));
    REQUIRE(reloadedDoc);
    CHECK(errorCount(*reloadedDoc) == 0);
    const auto names = allOfClass(*reloadedDoc, "CompositionMob");
    CHECK(std::ranges::any_of(names, [&](ObjectId id) { return stringValue(*reloadedDoc, id, "Name") == "New composition"; }));

    REQUIRE(session.undo());
    CHECK(sameAs(doc, before).empty());
    REQUIRE(session.redo());
    CHECK(doc.isAttached(mob));
}

TEST_CASE("Changing a definition's identifier updates weak references to it", "[edit][session]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto before = snapshot(doc);
    const auto definitions = allOfClass(doc, "DataDefinition");
    ObjectId used = kNoObject;
    const auto dataPid = pid(doc, firstOfClass(doc, "SourceClip"), "DataDefinition");
    for (std::size_t i = 0; i < doc.objectCount() && used == kNoObject; ++i)
    {
        if (const auto* p = doc.object(i).find(dataPid))
        {
            const auto& w = std::get<WeakRefProperty>(p->payload);
            used = doc.resolveWeak(w.tag, w.key).value_or(kNoObject);
        }
    }
    REQUIRE(used != kNoObject);

    const auto newId = "11111111-2222-3333-4444-555555555555"_auid;
    const auto changes = session.execute("Re-identify", [&](Transaction& tx) { return setProperty(tx, used, pid(doc, used, "Identification"), Value(newId)); });
    REQUIRE(changes);
    CHECK(changes->objects.size() > 2);
    CHECK(errorCount(doc) == 0);
    std::size_t pointing = 0;
    for (std::size_t i = 0; i < doc.objectCount(); ++i)
    {
        if (const auto* p = doc.object(i).find(dataPid); p != nullptr && doc.isAttached(i))
        {
            const auto& w = std::get<WeakRefProperty>(p->payload);
            REQUIRE(doc.resolveWeak(w.tag, w.key));
            pointing += doc.resolveWeak(w.tag, w.key) == used ? 1U : 0U;
        }
    }
    CHECK(pointing > 0);
    CHECK_FALSE(session.execute("Duplicate identifier", [&](Transaction& tx) {
        const auto other = definitions.front() == used ? definitions.back() : definitions.front();
        return setProperty(tx, other, pid(doc, other, "Identification"), Value(newId));
    }));
    REQUIRE(session.undo());
    CHECK(sameAs(doc, before).empty());
}

TEST_CASE("Deleting referenced objects requires force", "[edit][session]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto before = snapshot(doc);
    const auto clip = firstOfClass(doc, "SourceClip");
    const auto& w = std::get<WeakRefProperty>(doc.object(clip).find(pid(doc, clip, "DataDefinition"))->payload);
    const auto definition = doc.resolveWeak(w.tag, w.key).value();

    const auto refused = session.execute("Delete", [&](Transaction& tx) { return deleteObject(tx, definition); });
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("weak reference") != std::string::npos);
    REQUIRE(session.execute("Delete anyway", [&](Transaction& tx) { return deleteObject(tx, definition, true); }));
    CHECK_FALSE(doc.isAttached(definition));
    CHECK(errorCount(doc) > 0);
    REQUIRE(session.undo());
    CHECK(sameAs(doc, before).empty());
    CHECK(errorCount(doc) == 0);
}

TEST_CASE("Vector elements can be moved, removed and reinserted", "[edit][session]")
{
    auto opened = openSession();
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto before = snapshot(doc);
    ObjectId sequence = kNoObject;
    for (const auto id : allOfClass(doc, "Sequence"))
    {
        if (std::get<StrongRefVectorProperty>(doc.object(id).find(pid(doc, id, "Components"))->payload).objects.size() >= 3)
        {
            sequence = id;
            break;
        }
    }
    REQUIRE(sequence != kNoObject);
    const auto components = pid(doc, sequence, "Components");
    const auto original = std::get<StrongRefVectorProperty>(doc.object(sequence).find(components)->payload).objects;

    REQUIRE(session.execute("Move", [&](Transaction& tx) { return moveInCollection(tx, sequence, components, 0, 2); }));
    auto now = std::get<StrongRefVectorProperty>(doc.object(sequence).find(components)->payload).objects;
    CHECK(now[2] == original[0]);
    CHECK(now[0] == original[1]);

    REQUIRE(session.execute("Remove and reinsert", [&](Transaction& tx) -> Result<void> {
        auto removed = removeFromCollection(tx, sequence, components, 1);
        if (!removed)
        {
            return std::unexpected(removed.error());
        }
        return insertIntoCollection(tx, sequence, components, 0, *removed);
    }));
    now = std::get<StrongRefVectorProperty>(doc.object(sequence).find(components)->payload).objects;
    CHECK(now.front() == original[2]);
    CHECK(errorCount(doc) == 0);

    REQUIRE(session.undo());
    REQUIRE(session.undo());
    CHECK(sameAs(doc, before).empty());
    CHECK(saveToMemory(doc).size() > 0);
}

TEST_CASE("Stream data can be replaced and saved", "[edit][session]")
{
    auto opened = openSession("aafsdk/examples/com-api/ExportPCM/ExportPCM_NoCodecDef.aaf");
    REQUIRE(opened);
    auto& session = *opened;
    const auto& doc = session.document();
    const auto essence = firstOfClass(doc, "EssenceData");
    const auto dataPid = pid(doc, essence, "Data");
    const auto payload = bytesOf("new essence bytes");
    REQUIRE(session.execute("Replace essence", [&](Transaction& tx) { return setStreamData(tx, essence, dataPid, payload); }));

    auto reloaded = Document::load(openMemory(saveToMemory(doc)).value());
    REQUIRE(reloaded);
    const auto essences = allOfClass(*reloaded, "EssenceData");
    const auto found = std::ranges::any_of(essences, [&](ObjectId id) {
        const auto& s = std::get<StreamProperty>(reloaded->object(id).find(dataPid)->payload);
        return reloaded->container().openStream(s.entry).value().readAll().value() == payload;
    });
    CHECK(found);
}

TEST_CASE("Saving keeps history and tracks the saved position", "[edit][session]")
{
    const auto dir = std::filesystem::temp_directory_path() / std::format("aaf-session-{}", std::random_device{}());
    std::filesystem::create_directories(dir);
    const auto path = dir / "edited.aaf";
    {
        auto opened = openSession();
        REQUIRE(opened);
        auto& session = *opened;
        const auto& doc = session.document();
        const auto mob = firstOfClass(doc, "CompositionMob");
        REQUIRE(session.execute("Rename", [&](Transaction& tx) { return setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::string("Saved name"))); }));
        REQUIRE(session.save(path));
        CHECK_FALSE(session.dirty());
        CHECK(session.canUndo());
        REQUIRE(session.undo());
        CHECK(session.dirty());
        REQUIRE(session.execute("Rename again", [&](Transaction& tx) { return setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::string("Other"))); }));
        REQUIRE_FALSE(session.canRedo());
        REQUIRE(session.undo());
        CHECK(session.dirty());

        auto saved = Document::open(path);
        REQUIRE(saved);
        const auto mobs = allOfClass(*saved, "CompositionMob");
        CHECK(std::ranges::any_of(mobs, [&](ObjectId id) { return stringValue(*saved, id, "Name") == "Saved name"; }));
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("Random edit sequences undo and redo symmetrically", "[edit][session][symmetry]")
{
    for (const auto* fixture : { "aafsdk/test/com/MemoryLeakTest/RealWorldSample1.aaf", "aafsdk/examples/com-api/ExportDV/ExportDV_NoCodecDef.aaf" })
    {
        INFO(fixture);
        auto opened = openSession(fixture);
        REQUIRE(opened);
        auto& session = *opened;
        const auto& doc = session.document();
        const auto original = snapshot(doc);
        const auto originalBytes = saveToMemory(doc);
        std::mt19937 rng(42);
        auto pick = [&](const std::vector<ObjectId>& ids) { return ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)]; };

        std::size_t applied = 0;
        for (int step = 0; step < 60; ++step)
        {
            const auto kind = rng() % 5;
            const auto mobs = allOfClass(doc, "MasterMob").empty() ? allOfClass(doc, "SourceMob") : allOfClass(doc, "MasterMob");
            const auto sequences = allOfClass(doc, "Sequence");
            Session::Command command;
            if (kind == 0 && !mobs.empty())
            {
                const auto mob = pick(mobs);
                command = [&, mob, step](Transaction& tx) { return setProperty(tx, mob, pid(doc, mob, "Name"), Value(std::format("name {}", step))); };
            }
            else if (kind == 1 && !mobs.empty())
            {
                const auto mob = pick(mobs);
                command = [&, mob, step](Transaction& tx) { return setProperty(tx, mob, pid(doc, mob, "MobID"), Value(newMobId(static_cast<std::uint8_t>(step)))); };
            }
            else if (kind == 2 && !sequences.empty())
            {
                const auto seq = pick(sequences);
                command = [&, seq](Transaction& tx) -> Result<void> {
                    const auto components = pid(doc, seq, "Components");
                    const auto size = std::get<StrongRefVectorProperty>(doc.object(seq).find(components)->payload).objects.size();
                    return size < 2 ? Result<void>{} : moveInCollection(tx, seq, components, 0, size - 1);
                };
            }
            else if (kind == 3 && !mobs.empty())
            {
                const auto mob = pick(mobs);
                command = [&, mob](Transaction& tx) { return deleteObject(tx, mob); };
            }
            else if (session.canUndo())
            {
                REQUIRE(session.undo());
                continue;
            }
            else
            {
                continue;
            }
            if (session.execute(std::format("step {}", step), command))
            {
                ++applied;
            }
            REQUIRE(errorCount(doc) == 0);
        }
        CHECK(applied > 10);

        const auto edited = snapshot(doc);
        const auto position = session.position();
        while (session.canUndo())
        {
            REQUIRE(session.undo());
        }
        CHECK(sameAs(doc, original) == "");
        auto restored = openMemory(saveToMemory(doc));
        auto source = openMemory(originalBytes);
        REQUIRE(restored);
        REQUIRE(source);
        CHECK(diffTrees(*source, *restored) == "");
        for (std::size_t i = 0; i < position; ++i)
        {
            REQUIRE(session.redo());
        }
        CHECK(sameAs(doc, edited) == "");
    }
}
