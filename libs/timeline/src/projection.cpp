#include "access.hpp"

#include <aaf/timeline/timeline.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>

namespace aaf::timeline
{

namespace
{

using detail::Access;

constexpr int kMaxDepth = 16;
constexpr std::size_t kMaxChain = 64;

using namespace literals;

struct KnownDefinition
{
    Auid id;
    TrackKind kind = TrackKind::other;
};

constexpr std::array<KnownDefinition, 14> kDataDefinitions = { {
    { "01030202-0100-0000-060e-2b3404010101"_auid, TrackKind::picture },
    { "6f3c8ce1-6cef-11d2-807d-006008143e6f"_auid, TrackKind::picture },
    { "05cba731-1daa-11d3-80ad-006008143e6f"_auid, TrackKind::picture },
    { "05cba732-1daa-11d3-80ad-006008143e6f"_auid, TrackKind::picture },
    { "01030202-0200-0000-060e-2b3404010101"_auid, TrackKind::sound },
    { "78e1ebe1-6cef-11d2-807d-006008143e6f"_auid, TrackKind::sound },
    { "01030201-0100-0000-060e-2b3404010101"_auid, TrackKind::timecode },
    { "01030201-0200-0000-060e-2b3404010101"_auid, TrackKind::timecode },
    { "01030201-0300-0000-060e-2b3404010101"_auid, TrackKind::timecode },
    { "7f275e81-77e5-11d2-807f-006008143e6f"_auid, TrackKind::timecode },
    { "d2bb2af0-d234-11d2-89ee-006097116212"_auid, TrackKind::edgecode },
    { "01030201-1000-0000-060e-2b3404010101"_auid, TrackKind::descriptiveMetadata },
    { "01030202-0300-0000-060e-2b3404010101"_auid, TrackKind::data },
    { "01030203-0100-0000-060e-2b3404010105"_auid, TrackKind::data },
} };

auto kindOfItem(const Access& a, ObjectId id) -> ItemKind
{
    static constexpr std::array<std::pair<std::string_view, ItemKind>, 15> kKinds = { {
        { "SourceClip", ItemKind::sourceClip },
        { "Filler", ItemKind::filler },
        { "Transition", ItemKind::transition },
        { "OperationGroup", ItemKind::operationGroup },
        { "EssenceGroup", ItemKind::essenceGroup },
        { "Selector", ItemKind::selector },
        { "NestedScope", ItemKind::nestedScope },
        { "ScopeReference", ItemKind::scopeReference },
        { "Pulldown", ItemKind::pulldown },
        { "Sequence", ItemKind::sequence },
        { "Timecode", ItemKind::timecode },
        { "EdgeCode", ItemKind::edgecode },
        { "DescriptiveMarker", ItemKind::marker },
        { "CommentMarker", ItemKind::marker },
        { "Event", ItemKind::event },
    } };
    for (const auto& [name, kind] : kKinds)
    {
        if (a.isA(id, name))
        {
            return kind;
        }
    }
    return ItemKind::other;
}

auto readTimecode(const Access& a, ObjectId id) -> Timecode
{
    Timecode tc;
    tc.start = a.integer(id, "Timecode", "Start").value_or(0);
    tc.fps = static_cast<std::uint32_t>(a.integer(id, "Timecode", "FPS").value_or(0));
    const auto drop = a.value(id, "Timecode", "Drop");
    tc.drop = drop && drop->is<bool>() && drop->as<bool>();
    return tc;
}

auto nullMobId(const MobId& id) -> bool
{
    return id == MobId{};
}

}

auto to_string(MobKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
        case MobKind::composition:
            return "composition";
        case MobKind::master:
            return "master";
        case MobKind::source:
            return "source";
        case MobKind::other:
            break;
    }
    return "other";
}

auto to_string(TrackKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
        case TrackKind::picture:
            return "picture";
        case TrackKind::sound:
            return "sound";
        case TrackKind::timecode:
            return "timecode";
        case TrackKind::edgecode:
            return "edgecode";
        case TrackKind::descriptiveMetadata:
            return "descriptiveMetadata";
        case TrackKind::data:
            return "data";
        case TrackKind::other:
            break;
    }
    return "other";
}

auto to_string(SlotKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
        case SlotKind::timeline:
            return "timeline";
        case SlotKind::event:
            return "event";
        case SlotKind::fixed:
            return "static";
    }
    return "timeline";
}

auto to_string(ItemKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
        case ItemKind::sourceClip:
            return "sourceClip";
        case ItemKind::filler:
            return "filler";
        case ItemKind::transition:
            return "transition";
        case ItemKind::operationGroup:
            return "operationGroup";
        case ItemKind::essenceGroup:
            return "essenceGroup";
        case ItemKind::selector:
            return "selector";
        case ItemKind::nestedScope:
            return "nestedScope";
        case ItemKind::scopeReference:
            return "scopeReference";
        case ItemKind::pulldown:
            return "pulldown";
        case ItemKind::sequence:
            return "sequence";
        case ItemKind::timecode:
            return "timecode";
        case ItemKind::edgecode:
            return "edgecode";
        case ItemKind::marker:
            return "marker";
        case ItemKind::event:
            return "event";
        case ItemKind::other:
            break;
    }
    return "other";
}

auto to_string(ChainStatus status) noexcept -> std::string_view
{
    switch (status)
    {
        case ChainStatus::resolved:
            return "resolved";
        case ChainStatus::missingMob:
            return "missingMob";
        case ChainStatus::missingSlot:
            return "missingSlot";
        case ChainStatus::cycle:
            return "cycle";
    }
    return "resolved";
}

Projector::Projector(const Document& document) :
    doc_(document)
{
    const Access a(doc_);
    for (std::size_t i = 0; i < doc_.objectCount(); ++i)
    {
        const bool mob = a.isA(i, "Mob");
        const bool essence = !mob && a.isA(i, "EssenceData");
        if ((!mob && !essence) || !doc_.isAttached(i))
        {
            continue;
        }
        const auto id = a.value(i, mob ? "Mob" : "EssenceData", "MobID");
        if (id && id->is<MobId>())
        {
            (mob ? mobIndex_ : essenceIndex_).emplace_back(id->as<MobId>(), i);
        }
    }
    std::ranges::sort(mobIndex_);
    std::ranges::sort(essenceIndex_);
}

auto Projector::findMob(const MobId& id) const -> std::optional<ObjectId>
{
    const auto it = std::ranges::lower_bound(mobIndex_, id, {}, &std::pair<MobId, ObjectId>::first);
    return it != mobIndex_.end() && it->first == id ? std::optional(it->second) : std::nullopt;
}

auto Projector::mobKind(ObjectId mob) const -> MobKind
{
    const Access a(doc_);
    if (a.isA(mob, "CompositionMob"))
    {
        return MobKind::composition;
    }
    if (a.isA(mob, "MasterMob"))
    {
        return MobKind::master;
    }
    if (a.isA(mob, "SourceMob"))
    {
        return MobKind::source;
    }
    return MobKind::other;
}

auto Projector::trackKindOf(ObjectId component) const -> TrackKind
{
    const Access a(doc_);
    const auto key = a.weakKey(component, "Component", "DataDefinition");
    if (key)
    {
        for (const auto& known : kDataDefinitions)
        {
            if (known.id == *key)
            {
                return known.kind;
            }
        }
    }
    const auto name = a.definitionName(a.weak(component, "Component", "DataDefinition"));
    if (name.contains("Picture") || name.contains("Matte"))
    {
        return TrackKind::picture;
    }
    if (name.contains("Sound"))
    {
        return TrackKind::sound;
    }
    if (name.contains("Timecode"))
    {
        return TrackKind::timecode;
    }
    if (name.contains("Edgecode"))
    {
        return TrackKind::edgecode;
    }
    if (name.contains("Descriptive"))
    {
        return TrackKind::descriptiveMetadata;
    }
    return TrackKind::other;
}

auto Projector::mobs() const -> std::vector<MobSummary>
{
    const Access a(doc_);
    std::set<MobId> referenced;
    for (std::size_t i = 0; i < doc_.objectCount(); ++i)
    {
        if (a.isA(i, "SourceReference") && doc_.isAttached(i))
        {
            if (const auto v = a.value(i, "SourceReference", "SourceID"); v && v->is<MobId>())
            {
                referenced.insert(v->as<MobId>());
            }
        }
    }
    std::vector<MobSummary> out;
    for (const auto& [id, object] : mobIndex_)
    {
        MobSummary s;
        s.object = object;
        s.mobId = id;
        s.name = a.string(object, "Mob", "Name");
        s.kind = mobKind(object);
        s.tracks = a.children(object, "Mob", "Slots").size();
        if (s.kind == MobKind::composition)
        {
            const auto usage = a.value(object, "Mob", "UsageCode");
            if (usage && usage->is<Value::ExtEnum>())
            {
                s.topLevel = usage->as<Value::ExtEnum>().name == "Usage_TopLevel";
            }
            else
            {
                s.topLevel = !referenced.contains(id);
            }
        }
        out.push_back(std::move(s));
    }
    std::ranges::stable_sort(out, [](const MobSummary& x, const MobSummary& y) -> bool {
        if (x.topLevel != y.topLevel)
        {
            return x.topLevel;
        }
        if (x.kind != y.kind)
        {
            return x.kind < y.kind;
        }
        return x.name < y.name;
    });
    return out;
}

auto Projector::buildSequence(ObjectId segment, int depth, std::vector<std::string>& warnings) const -> std::vector<Item>
{
    const Access a(doc_);
    std::vector<Item> items;
    if (!a.isA(segment, "Sequence"))
    {
        items.push_back(buildItem(segment, 0, depth, warnings));
        return items;
    }
    std::int64_t cursor = 0;
    for (const auto component : a.children(segment, "Sequence", "Components"))
    {
        if (a.isA(component, "Event"))
        {
            items.push_back(buildItem(component, a.integer(component, "Event", "Position").value_or(0), depth, warnings));
            continue;
        }
        const auto length = a.integer(component, "Component", "Length").value_or(0);
        if (a.isA(component, "Transition"))
        {
            items.push_back(buildItem(component, cursor - length, depth, warnings));
            cursor -= length;
            continue;
        }
        items.push_back(buildItem(component, cursor, depth, warnings));
        cursor += length;
    }
    return items;
}

auto Projector::buildItem(ObjectId component, std::int64_t start, int depth, std::vector<std::string>& warnings) const -> Item
{
    const Access a(doc_);
    Item item;
    item.object = component;
    item.kind = kindOfItem(a, component);
    item.className = a.className(component);
    item.start = start;
    const auto length = a.integer(component, "Component", "Length");
    item.hasLength = length.has_value();
    item.length = length.value_or(0);
    if (depth > kMaxDepth)
    {
        warnings.push_back(std::format("nesting deeper than {} at object {}", kMaxDepth, component));
        return item;
    }
    auto nestedFrom = [&](ObjectId segment) -> void { item.nested.push_back(buildSequence(segment, depth + 1, warnings)); };
    switch (item.kind)
    {
        case ItemKind::sourceClip:
        {
            SourceReference ref;
            if (const auto v = a.value(component, "SourceReference", "SourceID"); v && v->is<MobId>())
            {
                ref.mobId = v->as<MobId>();
            }
            ref.slotId = static_cast<std::uint32_t>(a.integer(component, "SourceReference", "SourceMobSlotID").value_or(0));
            ref.startTime = a.integer(component, "SourceClip", "StartTime").value_or(0);
            ref.original = nullMobId(ref.mobId);
            if (!ref.original)
            {
                ref.mob = findMob(ref.mobId);
                if (ref.mob)
                {
                    ref.mobName = a.string(*ref.mob, "Mob", "Name");
                    ref.mobKind = mobKind(*ref.mob);
                }
            }
            if (!ref.mobName.empty())
            {
                item.label = ref.mobName;
            }
            else
            {
                item.label = ref.original ? "Original source" : "Missing source";
            }
            item.source = std::move(ref);
            break;
        }
        case ItemKind::filler:
            item.label = "Filler";
            break;
        case ItemKind::transition:
        {
            const auto group = a.child(component, "Transition", "OperationGroup");
            item.effect = group ? a.definitionName(a.weak(*group, "OperationGroup", "Operation")) : std::string{};
            item.label = item.effect.empty() ? "Transition" : item.effect;
            break;
        }
        case ItemKind::operationGroup:
            item.effect = a.definitionName(a.weak(component, "OperationGroup", "Operation"));
            item.label = item.effect.empty() ? "Effect" : item.effect;
            for (const auto input : a.children(component, "OperationGroup", "InputSegments"))
            {
                nestedFrom(input);
            }
            break;
        case ItemKind::essenceGroup:
            item.label = "Essence group";
            for (const auto choice : a.children(component, "EssenceGroup", "Choices"))
            {
                nestedFrom(choice);
            }
            break;
        case ItemKind::selector:
            item.label = "Selector";
            if (const auto selected = a.child(component, "Selector", "Selected"))
            {
                nestedFrom(*selected);
            }
            for (const auto alternate : a.children(component, "Selector", "Alternates"))
            {
                nestedFrom(alternate);
            }
            break;
        case ItemKind::nestedScope:
            item.label = "Nested scope";
            for (const auto slot : a.children(component, "NestedScope", "Slots"))
            {
                nestedFrom(slot);
            }
            break;
        case ItemKind::pulldown:
            item.label = "Pulldown";
            if (const auto input = a.child(component, "Pulldown", "InputSegment"))
            {
                nestedFrom(*input);
            }
            break;
        case ItemKind::sequence:
            item.label = "Sequence";
            item.nested.push_back(buildSequence(component, depth + 1, warnings));
            break;
        case ItemKind::timecode:
            item.timecode = readTimecode(a, component);
            item.label = "Timecode";
            break;
        case ItemKind::marker:
        case ItemKind::event:
            item.comment = a.string(component, "Event", "Comment");
            item.label = item.comment.empty() ? a.className(component) : item.comment;
            break;
        case ItemKind::scopeReference:
            item.label = "Scope reference";
            break;
        case ItemKind::edgecode:
            item.label = "Edgecode";
            break;
        case ItemKind::other:
            item.label = item.className;
            break;
    }
    return item;
}

auto Projector::project(ObjectId mob) const -> Result<MobTimeline>
{
    const Access a(doc_);
    if (mob >= doc_.objectCount() || !a.isA(mob, "Mob"))
    {
        return fail(Errc::invalid_argument, std::format("object {} is not a mob", mob));
    }
    MobTimeline t;
    t.mob = mob;
    if (const auto id = a.value(mob, "Mob", "MobID"); id && id->is<MobId>())
    {
        t.mobId = id->as<MobId>();
    }
    t.name = a.string(mob, "Mob", "Name");
    t.kind = mobKind(mob);
    for (const auto slot : a.children(mob, "Mob", "Slots"))
    {
        Track track;
        track.slot = slot;
        track.slotId = static_cast<std::uint32_t>(a.integer(slot, "MobSlot", "SlotID").value_or(0));
        track.name = a.string(slot, "MobSlot", "SlotName");
        if (const auto number = a.integer(slot, "MobSlot", "PhysicalTrackNumber"))
        {
            track.physicalNumber = static_cast<std::uint32_t>(*number);
        }
        if (a.isA(slot, "TimelineMobSlot"))
        {
            track.slotKind = SlotKind::timeline;
            track.editRate = a.rational(slot, "TimelineMobSlot", "EditRate").value_or(Rational(25, 1));
            track.origin = a.integer(slot, "TimelineMobSlot", "Origin").value_or(0);
        }
        else if (a.isA(slot, "EventMobSlot"))
        {
            track.slotKind = SlotKind::event;
            track.editRate = a.rational(slot, "EventMobSlot", "EditRate").value_or(Rational(25, 1));
        }
        else
        {
            track.slotKind = SlotKind::fixed;
        }
        const auto segment = a.child(slot, "MobSlot", "Segment");
        if (!segment)
        {
            t.warnings.push_back(std::format("slot {} has no segment", track.slotId));
            t.tracks.push_back(std::move(track));
            continue;
        }
        track.segment = *segment;
        track.kind = trackKindOf(*segment);
        ObjectId content = *segment;
        for (int depth = 0; depth < kMaxDepth && a.isA(content, "OperationGroup"); ++depth)
        {
            const auto inputs = a.children(content, "OperationGroup", "InputSegments");
            if (inputs.empty())
            {
                break;
            }
            track.effects.push_back({ content, a.definitionName(a.weak(content, "OperationGroup", "Operation")) });
            content = inputs.front();
        }
        track.items = buildSequence(content, 0, t.warnings);
        const auto declared = a.integer(*segment, "Component", "Length");
        std::int64_t end = 0;
        for (const auto& item : track.items)
        {
            end = std::max(end, item.start + item.length);
        }
        track.length = declared.value_or(end);
        if (!t.timecode && track.kind == TrackKind::timecode)
        {
            for (const auto& item : track.items)
            {
                if (item.timecode)
                {
                    t.timecode = item.timecode;
                    break;
                }
            }
        }
        t.tracks.push_back(std::move(track));
    }
    return t;
}

auto Projector::resolve(ObjectId sourceClip) const -> Result<SourceChain>
{
    const Access a(doc_);
    if (sourceClip >= doc_.objectCount() || !a.isA(sourceClip, "SourceClip"))
    {
        return fail(Errc::invalid_argument, std::format("object {} is not a source clip", sourceClip));
    }
    SourceChain chain;
    std::set<std::pair<ObjectId, std::uint32_t>> visited;
    ObjectId clip = sourceClip;
    Rational rate(25, 1);
    for (ObjectId p = doc_.object(clip).parent; p != kNoObject; p = doc_.object(p).parent)
    {
        if (a.isA(p, "TimelineMobSlot"))
        {
            rate = a.rational(p, "TimelineMobSlot", "EditRate").value_or(rate);
            break;
        }
        if (a.isA(p, "EventMobSlot"))
        {
            rate = a.rational(p, "EventMobSlot", "EditRate").value_or(rate);
            break;
        }
    }
    std::int64_t offset = 0;
    while (chain.links.size() < kMaxChain)
    {
        const auto id = a.value(clip, "SourceReference", "SourceID");
        if (!id || !id->is<MobId>() || nullMobId(id->as<MobId>()))
        {
            break;
        }
        const auto slotId = static_cast<std::uint32_t>(a.integer(clip, "SourceReference", "SourceMobSlotID").value_or(0));
        const auto mob = findMob(id->as<MobId>());
        if (!mob)
        {
            chain.status = ChainStatus::missingMob;
            chain.links.push_back({ kNoObject, id->as<MobId>(), {}, MobKind::other, slotId, 0, rate, {} });
            break;
        }
        std::optional<ObjectId> slot;
        for (const auto s : a.children(*mob, "Mob", "Slots"))
        {
            if (a.integer(s, "MobSlot", "SlotID") == static_cast<std::int64_t>(slotId))
            {
                slot = s;
                break;
            }
        }
        if (!visited.emplace(*mob, slotId).second)
        {
            chain.status = ChainStatus::cycle;
            break;
        }
        ChainLink link{ *mob, id->as<MobId>(), a.string(*mob, "Mob", "Name"), mobKind(*mob), slotId, 0, rate, {} };
        if (const auto descriptor = a.child(*mob, "SourceMob", "EssenceDescription"))
        {
            link.descriptor = a.className(*descriptor);
            if (a.isA(*descriptor, "FileDescriptor"))
            {
                EssenceInfo essence;
                essence.descriptor = link.descriptor;
                const auto embedded = std::ranges::lower_bound(essenceIndex_, id->as<MobId>(), {}, &std::pair<MobId, ObjectId>::first);
                if (embedded != essenceIndex_.end() && embedded->first == id->as<MobId>())
                {
                    essence.embedded = true;
                    essence.essenceData = embedded->second;
                }
                for (const auto locator : a.children(*descriptor, "EssenceDescriptor", "Locator"))
                {
                    if (auto url = a.string(locator, "NetworkLocator", "URLString"); !url.empty())
                    {
                        essence.locators.push_back(std::move(url));
                    }
                }
                chain.essence = std::move(essence);
            }
        }
        if (!slot)
        {
            chain.status = ChainStatus::missingSlot;
            chain.links.push_back(std::move(link));
            break;
        }
        const auto slotRate = a.rational(*slot, "TimelineMobSlot", "EditRate").value_or(rate);
        const auto start = a.integer(clip, "SourceClip", "StartTime").value_or(0);
        auto position = convertPosition(start + offset, rate, slotRate);
        if (!position)
        {
            return std::unexpected(position.error());
        }
        link.position = *position;
        link.editRate = slotRate;
        chain.links.push_back(std::move(link));

        const auto segment = a.child(*slot, "MobSlot", "Segment");
        if (!segment)
        {
            break;
        }
        std::vector<std::string> ignored;
        const auto items = buildSequence(*segment, kMaxDepth, ignored);
        const auto found = std::ranges::find_if(items, [&](const Item& item) -> bool { return item.kind == ItemKind::sourceClip && *position >= item.start && *position < item.start + std::max<std::int64_t>(item.length, 1); });
        if (found == items.end())
        {
            break;
        }
        offset = *position - found->start;
        clip = found->object;
        rate = slotRate;
    }
    return chain;
}

}
