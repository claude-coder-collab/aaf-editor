#include "access.hpp"

#include <aaf/edit/operations.hpp>
#include <aaf/timeline/edit.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace aaf::timeline::ops
{

namespace
{

using detail::Access;
using namespace literals;

constexpr int kMaxDepth = 16;

struct TrackRef
{
    ObjectId slot = kNoObject;
    ObjectId sequence = kNoObject;
    std::vector<ObjectId> wrappers;
};

struct Entry
{
    ObjectId id = kNoObject;
    bool transition = false;
    bool filler = false;
    /// A source clip, or single-input effects around one (see ClipChain).
    bool clip = false;
    /// Why the clip's length cannot be changed, if it cannot.
    std::string locked;
    std::int64_t start = 0;
    std::int64_t length = 0;
};

/// A source clip, or a chain of single-input effects around one, edited as one unit. A multichannel clip is an
/// audio channel combiner (possibly inside single-input effects) whose every input is such a chain: one source
/// clip per channel.
struct ClipChain
{
    /// Every OperationGroup in the unit: the effects from the track inwards, the channel combiner, and each
    /// channel's own effects. Empty for a plain source clip.
    std::vector<ObjectId> effects;
    /// The source clips, one per channel.
    std::vector<ObjectId> clips;
    /// Set when changing the length would change what the effects do: speed changes map clip time differently,
    /// and keyframe times are relative to the effect's length.
    std::string locked;
};

auto collectChain(const Access& a, ObjectId node, bool allowCombiner, int depth, ClipChain& chain) -> bool
{
    if (depth >= kMaxDepth)
    {
        return false;
    }
    if (a.isA(node, "SourceClip"))
    {
        chain.clips.push_back(node);
        return true;
    }
    if (!a.isA(node, "OperationGroup"))
    {
        return false;
    }
    const auto inputs = a.children(node, "OperationGroup", "InputSegments");
    const bool combiner = allowCombiner && inputs.size() > 1 && a.isChannelCombiner(node);
    if (inputs.size() != 1 && !combiner)
    {
        return false;
    }
    const auto definition = a.weak(node, "OperationGroup", "Operation");
    const auto name = a.definitionName(definition);
    const auto warp = definition ? a.value(*definition, "OperationDefinition", "IsTimeWarp") : std::nullopt;
    if (chain.locked.empty() && warp && warp->is<bool>() && warp->as<bool>())
    {
        chain.locked = std::format("the clip has a speed change ({}), which cannot be trimmed or split yet", name.empty() ? "effect" : name);
    }
    const auto parameters = a.children(node, "OperationGroup", "Parameters");
    if (chain.locked.empty() && std::ranges::any_of(parameters, [&](ObjectId p) -> bool { return a.isA(p, "VaryingValue"); }))
    {
        chain.locked = std::format("the clip has keyframed effect parameters ({}), which cannot be trimmed or split yet", name.empty() ? "effect" : name);
    }
    chain.effects.push_back(node);
    return std::ranges::all_of(inputs, [&](ObjectId input) -> bool { return collectChain(a, input, allowCombiner && !combiner, depth + 1, chain); });
}

auto clipChain(const Access& a, ObjectId id) -> std::optional<ClipChain>
{
    ClipChain chain;
    if (!collectChain(a, id, true, 0, chain))
    {
        return std::nullopt;
    }
    return chain;
}

/// The source clips that decide where a segment's material comes from: itself, or the clips inside its effects
/// (one per channel for a multichannel clip).
auto sourceClipsOf(const Access& a, ObjectId id) -> std::vector<ObjectId>
{
    const auto chain = clipChain(a, id);
    return chain ? chain->clips : std::vector<ObjectId>{ id };
}

auto pidOf(const Access& a, std::string_view cls, std::string_view name) -> Result<std::uint16_t>
{
    const auto* def = a.model_.findProperty(cls, name);
    if (def == nullptr || def->pid == 0)
    {
        return fail(Errc::not_found, std::format("the model has no {}.{}", cls, name));
    }
    return def->pid;
}

template <typename T>
auto propagate(const Result<T>& r) -> std::unexpected<Error>
{
    return std::unexpected(r.error());
}

auto lengthOf(const Access& a, ObjectId id) -> std::int64_t
{
    return a.integer(id, "Component", "Length").value_or(0);
}

auto setInt(edit::Transaction& tx, ObjectId id, std::string_view cls, std::string_view name, std::int64_t value) -> Result<void>
{
    const Access a(tx.document());
    auto pid = pidOf(a, cls, name);
    if (!pid)
    {
        return propagate(pid);
    }
    return edit::setProperty(tx, id, *pid, Value(value));
}

auto setLength(edit::Transaction& tx, ObjectId id, std::int64_t length) -> Result<void>
{
    return setInt(tx, id, "Component", "Length", length);
}

/// Sets a segment's length; for a clip inside effects, the effects and the clip all get the new length, and for a
/// multichannel clip, every channel does.
auto setSegmentLength(edit::Transaction& tx, ObjectId id, std::int64_t length) -> Result<void>
{
    const Access a(tx.document());
    const auto chain = clipChain(a, id);
    if (!chain || chain->effects.empty())
    {
        return setLength(tx, id, length);
    }
    if (!chain->locked.empty())
    {
        return fail(Errc::unsupported, chain->locked);
    }
    for (const auto* parts : { &chain->effects, &chain->clips })
    {
        for (const auto part : *parts)
        {
            if (auto r = setLength(tx, part, length); !r)
            {
                return r;
            }
        }
    }
    return {};
}

/// Copies the DataDefinition weak reference of `from` onto `to`.
auto copyDataDefinition(edit::Transaction& tx, ObjectId from, ObjectId to) -> Result<void>
{
    const Access a(tx.document());
    auto pid = pidOf(a, "Component", "DataDefinition");
    if (!pid)
    {
        return propagate(pid);
    }
    const auto* p = tx.document().object(from).find(*pid);
    if (p == nullptr)
    {
        return fail(Errc::invalid_argument, "the track has no data definition");
    }
    auto copy = *p;
    auto& target = tx.touch(to);
    std::erase_if(target.properties, [&](const Property& x) -> bool { return x.pid == copy.pid; });
    target.properties.push_back(std::move(copy));
    return {};
}

auto components(const Access& a, ObjectId sequence) -> std::vector<ObjectId>
{
    return a.children(sequence, "Sequence", "Components");
}

auto entries(const Access& a, ObjectId sequence) -> std::vector<Entry>
{
    std::vector<Entry> out;
    std::int64_t cursor = 0;
    for (const auto id : components(a, sequence))
    {
        const auto chain = clipChain(a, id);
        Entry e{ id, a.isA(id, "Transition"), a.isA(id, "Filler"), chain.has_value(), chain ? chain->locked : std::string{}, cursor, lengthOf(a, id) };
        if (e.transition)
        {
            e.start = cursor - e.length;
            cursor -= e.length;
        }
        else
        {
            cursor += e.length;
        }
        out.push_back(e);
    }
    return out;
}

auto total(const std::vector<Entry>& list) -> std::int64_t
{
    std::int64_t sum = 0;
    for (const auto& e : list)
    {
        sum += e.transition ? -e.length : e.length;
    }
    return sum;
}

auto openTrack(edit::Transaction& tx, ObjectId slot) -> Result<TrackRef>
{
    const auto& doc = tx.document();
    const Access a(doc);
    if (slot >= doc.objectCount() || !a.isA(slot, "MobSlot") || !doc.isAttached(slot))
    {
        return fail(Errc::invalid_argument, std::format("object {} is not a track", slot));
    }
    if (a.isA(slot, "EventMobSlot"))
    {
        return fail(Errc::invalid_argument, "marker tracks are edited with the marker operations");
    }
    const auto segment = a.child(slot, "MobSlot", "Segment");
    if (!segment)
    {
        return fail(Errc::invalid_argument, "the track has no segment");
    }
    TrackRef track{ slot, *segment, {} };
    for (int depth = 0; depth < kMaxDepth && a.isA(track.sequence, "OperationGroup"); ++depth)
    {
        const auto inputs = a.children(track.sequence, "OperationGroup", "InputSegments");
        if (inputs.empty())
        {
            break;
        }
        track.wrappers.push_back(track.sequence);
        track.sequence = inputs.front();
    }
    if (a.isA(track.sequence, "Sequence"))
    {
        return track;
    }
    if (!a.isA(track.sequence, "SourceClip") && !a.isA(track.sequence, "Filler"))
    {
        return fail(Errc::unsupported, std::format("tracks holding a {} cannot be edited", a.className(track.sequence)));
    }
    const auto content = track.sequence;
    auto sequence = edit::createObject(tx, a.model_.findClassByName("Sequence")->id);
    if (!sequence)
    {
        return propagate(sequence);
    }
    auto componentsPid = pidOf(a, "Sequence", "Components");
    auto segmentPid = pidOf(a, "MobSlot", "Segment");
    auto inputsPid = pidOf(a, "OperationGroup", "InputSegments");
    if (!componentsPid || !segmentPid || !inputsPid)
    {
        return fail(Errc::not_found, "the model lacks Sequence or MobSlot properties");
    }
    const auto length = lengthOf(a, content);
    for (const auto& r : { copyDataDefinition(tx, content, *sequence), edit::ensureCollection(tx, *sequence, *componentsPid), setLength(tx, *sequence, length) })
    {
        if (!r)
        {
            return propagate(r);
        }
    }
    if (track.wrappers.empty())
    {
        if (auto r = edit::setStrongRef(tx, slot, *segmentPid, *sequence); !r)
        {
            return propagate(r);
        }
    }
    else
    {
        auto removed = edit::removeFromCollection(tx, track.wrappers.back(), *inputsPid, 0);
        if (!removed)
        {
            return propagate(removed);
        }
        if (auto r = edit::insertIntoCollection(tx, track.wrappers.back(), *inputsPid, 0, *sequence); !r)
        {
            return propagate(r);
        }
    }
    if (auto r = edit::insertIntoCollection(tx, *sequence, *componentsPid, 0, content); !r)
    {
        return propagate(r);
    }
    track.sequence = *sequence;
    return track;
}

/// Finds the track and index of a component that sits directly in a track's sequence.
auto locate(edit::Transaction& tx, ObjectId item) -> Result<std::pair<TrackRef, std::size_t>>
{
    const auto& doc = tx.document();
    const Access a(doc);
    if (item >= doc.objectCount() || !doc.isAttached(item))
    {
        return fail(Errc::invalid_argument, std::format("object {} is not on a track", item));
    }
    const auto parent = doc.object(item).parent;
    ObjectId slot = kNoObject;
    if (a.isA(item, "Segment") && a.isA(parent, "MobSlot"))
    {
        slot = parent;
    }
    else if (a.isA(parent, "Sequence"))
    {
        ObjectId node = doc.object(parent).parent;
        for (int depth = 0; depth < kMaxDepth && node != kNoObject && a.isA(node, "OperationGroup"); ++depth)
        {
            node = doc.object(node).parent;
        }
        if (node != kNoObject && a.isA(node, "MobSlot"))
        {
            slot = node;
        }
    }
    if (slot == kNoObject)
    {
        return fail(Errc::unsupported, "only items placed directly on a track can be edited here");
    }
    auto track = openTrack(tx, slot);
    if (!track)
    {
        return propagate(track);
    }
    const auto list = components(a, track->sequence);
    const auto it = std::ranges::find(list, item);
    if (it == list.end())
    {
        return fail(Errc::unsupported, "only items placed directly on a track can be edited here");
    }
    return std::pair{ *track, static_cast<std::size_t>(it - list.begin()) };
}

auto insertAt(edit::Transaction& tx, const TrackRef& track, std::size_t index, ObjectId object) -> Result<void>
{
    const Access a(tx.document());
    auto pid = pidOf(a, "Sequence", "Components");
    if (!pid)
    {
        return propagate(pid);
    }
    return edit::insertIntoCollection(tx, track.sequence, *pid, index, object);
}

auto removeAt(edit::Transaction& tx, const TrackRef& track, std::size_t index) -> Result<ObjectId>
{
    const Access a(tx.document());
    auto pid = pidOf(a, "Sequence", "Components");
    if (!pid)
    {
        return propagate(pid);
    }
    return edit::removeFromCollection(tx, track.sequence, *pid, index);
}

auto makeFiller(edit::Transaction& tx, const TrackRef& track, std::int64_t length) -> Result<ObjectId>
{
    const Access a(tx.document());
    auto filler = edit::createObject(tx, a.model_.findClassByName("Filler")->id);
    if (!filler)
    {
        return filler;
    }
    if (auto r = copyDataDefinition(tx, track.sequence, *filler); !r)
    {
        return propagate(r);
    }
    if (auto r = setLength(tx, *filler, length); !r)
    {
        return propagate(r);
    }
    return filler;
}

auto normalize(edit::Transaction& tx, const TrackRef& track) -> Result<void>
{
    const Access a(tx.document());
    bool changed = true;
    while (changed)
    {
        changed = false;
        const auto list = entries(a, track.sequence);
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].filler && list[i].length == 0)
            {
                if (auto r = removeAt(tx, track, i); !r)
                {
                    return propagate(r);
                }
                changed = true;
                break;
            }
            if (i + 1 < list.size() && list[i].filler && list[i + 1].filler)
            {
                if (auto r = setLength(tx, list[i].id, list[i].length + list[i + 1].length); !r)
                {
                    return r;
                }
                if (auto r = removeAt(tx, track, i + 1); !r)
                {
                    return propagate(r);
                }
                changed = true;
                break;
            }
        }
    }
    const auto length = total(entries(a, track.sequence));
    if (auto r = setLength(tx, track.sequence, length); !r)
    {
        return r;
    }
    for (const auto wrapper : track.wrappers)
    {
        if (auto r = setLength(tx, wrapper, length); !r)
        {
            return r;
        }
    }
    return {};
}

auto adjustStart(edit::Transaction& tx, ObjectId segment, std::int64_t delta) -> Result<void>
{
    const Access a(tx.document());
    for (const auto clip : sourceClipsOf(a, segment))
    {
        const auto start = a.integer(clip, "SourceClip", "StartTime").value_or(0) + delta;
        if (start < 0)
        {
            return fail(Errc::invalid_argument, "the edit would move the clip before the start of its source");
        }
        if (auto r = setInt(tx, clip, "SourceClip", "StartTime", start); !r)
        {
            return r;
        }
    }
    return {};
}

/// Length of the source slot a clip refers to, when it can be determined.
auto sourceLength(const Access& a, ObjectId clip) -> std::optional<std::int64_t>
{
    const auto id = a.value(clip, "SourceReference", "SourceID");
    if (!id || !id->is<MobId>() || id->as<MobId>() == MobId{})
    {
        return std::nullopt;
    }
    const auto slotId = a.integer(clip, "SourceReference", "SourceMobSlotID");
    for (std::size_t i = 0; i < a.doc_.objectCount(); ++i)
    {
        if (!a.isA(i, "Mob") || a.value(i, "Mob", "MobID") != *id)
        {
            continue;
        }
        for (const auto slot : a.children(i, "Mob", "Slots"))
        {
            if (a.integer(slot, "MobSlot", "SlotID") == slotId)
            {
                const auto segment = a.child(slot, "MobSlot", "Segment");
                return segment ? a.integer(*segment, "Component", "Length") : std::nullopt;
            }
        }
    }
    return std::nullopt;
}

auto checkWithinSource(const Access& a, ObjectId segment) -> Result<void>
{
    for (const auto clip : sourceClipsOf(a, segment))
    {
        if (!a.isA(clip, "SourceClip"))
        {
            continue;
        }
        const auto available = sourceLength(a, clip);
        const auto end = a.integer(clip, "SourceClip", "StartTime").value_or(0) + lengthOf(a, clip);
        if (available && end > *available)
        {
            return fail(Errc::invalid_argument, std::format("the clip would extend past the end of its source ({} units)", *available));
        }
    }
    return {};
}

/// Replaces the transition at `index` with a cut at its midpoint.
auto cutTransition(edit::Transaction& tx, const TrackRef& track, std::size_t index) -> Result<void>
{
    const Access a(tx.document());
    const auto list = entries(a, track.sequence);
    if (index == 0 || index + 1 >= list.size() || list[index - 1].transition || list[index + 1].transition)
    {
        return fail(Errc::unsupported, "the transition is not between two segments");
    }
    const auto length = list[index].length;
    const auto half = length / 2;
    const auto& previous = list[index - 1];
    const auto& next = list[index + 1];
    if (previous.length < length - half || next.length < half)
    {
        return fail(Errc::invalid_argument, "the segments around the transition are too short");
    }
    if (auto r = setSegmentLength(tx, previous.id, previous.length - (length - half)); !r)
    {
        return r;
    }
    if (auto r = setSegmentLength(tx, next.id, next.length - half); !r)
    {
        return r;
    }
    if (next.clip)
    {
        if (auto r = adjustStart(tx, next.id, half); !r)
        {
            return r;
        }
    }
    auto removed = removeAt(tx, track, index);
    return removed ? Result<void>{} : propagate(removed);
}

/// Makes a cut at `position` and returns the index of the first component at or after it.
auto cutAt(edit::Transaction& tx, const TrackRef& track, std::int64_t position) -> Result<std::size_t>
{
    const Access a(tx.document());
    if (position < 0)
    {
        return fail(Errc::invalid_argument, "positions cannot be negative");
    }
    auto list = entries(a, track.sequence);
    const auto end = total(list);
    if (position >= end)
    {
        if (position > end)
        {
            auto filler = makeFiller(tx, track, position - end);
            if (!filler)
            {
                return propagate(filler);
            }
            if (auto r = insertAt(tx, track, list.size(), *filler); !r)
            {
                return propagate(r);
            }
            return list.size() + 1;
        }
        return list.size();
    }
    for (const auto& e : list)
    {
        if (e.transition && position > e.start && position < e.start + e.length)
        {
            return fail(Errc::invalid_argument, "the position is inside a transition");
        }
    }
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto& e = list[i];
        if (e.transition)
        {
            continue;
        }
        if (position == e.start)
        {
            return i > 0 && list[i - 1].transition ? i - 1 : i;
        }
        if (position > e.start && position < e.start + e.length)
        {
            if (!e.clip && !e.filler)
            {
                return fail(Errc::unsupported, std::format("a {} cannot be split", a.className(e.id)));
            }
            if (!e.locked.empty())
            {
                return fail(Errc::unsupported, e.locked);
            }
            auto right = deepCopy(tx, e.id);
            if (!right)
            {
                return propagate(right);
            }
            const auto left = position - e.start;
            for (const auto& r : { setSegmentLength(tx, e.id, left), setSegmentLength(tx, *right, e.length - left) })
            {
                if (!r)
                {
                    return propagate(r);
                }
            }
            if (e.clip)
            {
                if (auto r = adjustStart(tx, *right, left); !r)
                {
                    return propagate(r);
                }
            }
            if (auto r = insertAt(tx, track, i + 1, *right); !r)
            {
                return propagate(r);
            }
            return i + 1;
        }
    }
    return list.size();
}

auto sameDataDefinition(const Access& a, ObjectId x, ObjectId y) -> bool
{
    const auto kx = a.weakKey(x, "Component", "DataDefinition");
    const auto ky = a.weakKey(y, "Component", "DataDefinition");
    if (!kx || !ky)
    {
        return false;
    }
    if (*kx == *ky)
    {
        return true;
    }
    const Projector projector(a.doc_);
    const auto kind = projector.trackKindOf(x);
    return (kind == TrackKind::picture || kind == TrackKind::sound) && kind == projector.trackKindOf(y);
}

auto findDataDefinition(const Access& a, std::initializer_list<Auid> preferred, std::string_view nameHint) -> std::optional<ObjectId>
{
    const auto& doc = a.doc_;
    std::optional<ObjectId> byName;
    for (const auto& wanted : preferred)
    {
        for (std::size_t i = 0; i < doc.objectCount(); ++i)
        {
            if (a.isA(i, "DataDefinition") && doc.isAttached(i))
            {
                const auto id = a.value(i, "DefinitionObject", "Identification");
                if (id && id->is<Auid>() && id->as<Auid>() == wanted)
                {
                    return i;
                }
            }
        }
    }
    for (std::size_t i = 0; i < doc.objectCount() && !byName; ++i)
    {
        if (a.isA(i, "DataDefinition") && doc.isAttached(i) && a.string(i, "DefinitionObject", "Name").contains(nameHint))
        {
            byName = i;
        }
    }
    return byName;
}

auto firstTimelineRate(const Access& a, ObjectId mob) -> Rational
{
    for (const auto slot : a.children(mob, "Mob", "Slots"))
    {
        if (const auto rate = a.rational(slot, "TimelineMobSlot", "EditRate"))
        {
            return *rate;
        }
    }
    return { 25, 1 };
}

auto nextSlotId(const Access& a, ObjectId mob) -> std::uint32_t
{
    std::int64_t highest = 0;
    for (const auto slot : a.children(mob, "Mob", "Slots"))
    {
        highest = std::max(highest, a.integer(slot, "MobSlot", "SlotID").value_or(0));
    }
    return static_cast<std::uint32_t>(highest + 1);
}

auto rateValue(const Rational& r) -> Value
{
    return Value(Value::Record{ { "Numerator", "Denominator" }, { Value(r.numerator()), Value(r.denominator()) } });
}

}

auto deepCopy(edit::Transaction& tx, ObjectId id) -> Result<ObjectId>
{
    const auto& doc = tx.document();
    if (id >= doc.objectCount())
    {
        return fail(Errc::invalid_argument, std::format("object {} does not exist", id));
    }
    const auto source = doc.object(id);
    auto copy = edit::createObject(tx, source.classId);
    if (!copy)
    {
        return copy;
    }
    for (const auto& p : source.properties)
    {
        if (const auto* strong = std::get_if<StrongRefProperty>(&p.payload))
        {
            auto child = deepCopy(tx, strong->object);
            if (!child)
            {
                return child;
            }
            if (auto r = edit::setStrongRef(tx, *copy, p.pid, *child); !r)
            {
                return propagate(r);
            }
        }
        else if (std::holds_alternative<StrongRefVectorProperty>(p.payload) || std::holds_alternative<StrongRefSetProperty>(p.payload))
        {
            const auto* vector = std::get_if<StrongRefVectorProperty>(&p.payload);
            const auto& objects = vector ? vector->objects : std::get<StrongRefSetProperty>(p.payload).objects;
            if (auto r = edit::ensureCollection(tx, *copy, p.pid); !r)
            {
                return propagate(r);
            }
            for (std::size_t i = 0; i < objects.size(); ++i)
            {
                auto child = deepCopy(tx, objects[i]);
                if (!child)
                {
                    return child;
                }
                if (auto r = edit::insertIntoCollection(tx, *copy, p.pid, i, *child); !r)
                {
                    return propagate(r);
                }
            }
        }
        else
        {
            tx.touch(*copy).properties.push_back(p);
        }
    }
    return copy;
}

auto split(edit::Transaction& tx, ObjectId slot, std::int64_t position) -> Result<ObjectId>
{
    auto track = openTrack(tx, slot);
    if (!track)
    {
        return propagate(track);
    }
    const Access a(tx.document());
    const auto before = entries(a, track->sequence);
    const bool inside = std::ranges::any_of(before, [&](const Entry& e) -> bool { return !e.transition && !e.filler && position > e.start && position < e.start + e.length; });
    if (!inside)
    {
        return fail(Errc::invalid_argument, "there is no clip to split at that position");
    }
    auto index = cutAt(tx, *track, position);
    if (!index)
    {
        return propagate(index);
    }
    const auto right = components(a, track->sequence).at(*index);
    if (auto r = normalize(tx, *track); !r)
    {
        return propagate(r);
    }
    return right;
}

auto lift(edit::Transaction& tx, ObjectId item) -> Result<void>
{
    auto found = locate(tx, item);
    if (!found)
    {
        return propagate(found);
    }
    auto& [track, index] = *found;
    const Access a(tx.document());
    if (a.isA(item, "Transition"))
    {
        if (auto r = cutTransition(tx, track, index); !r)
        {
            return r;
        }
        return normalize(tx, track);
    }
    auto filler = makeFiller(tx, track, lengthOf(a, item));
    if (!filler)
    {
        return propagate(filler);
    }
    if (auto r = removeAt(tx, track, index); !r)
    {
        return propagate(r);
    }
    if (auto r = insertAt(tx, track, index, *filler); !r)
    {
        return r;
    }
    return normalize(tx, track);
}

auto rippleDelete(edit::Transaction& tx, ObjectId item) -> Result<void>
{
    auto found = locate(tx, item);
    if (!found)
    {
        return propagate(found);
    }
    auto [track, index] = *found;
    const Access a(tx.document());
    if (a.isA(item, "Transition"))
    {
        if (auto r = cutTransition(tx, track, index); !r)
        {
            return r;
        }
        return normalize(tx, track);
    }
    auto list = entries(a, track.sequence);
    if (index + 1 < list.size() && list[index + 1].transition)
    {
        if (auto r = cutTransition(tx, track, index + 1); !r)
        {
            return r;
        }
    }
    if (index > 0 && list[index - 1].transition)
    {
        if (auto r = cutTransition(tx, track, index - 1); !r)
        {
            return r;
        }
        --index;
    }
    if (auto r = removeAt(tx, track, index); !r)
    {
        return propagate(r);
    }
    return normalize(tx, track);
}

auto trim(edit::Transaction& tx, ObjectId item, Edge edge, std::int64_t delta, bool ripple) -> Result<void>
{
    auto found = locate(tx, item);
    if (!found)
    {
        return propagate(found);
    }
    const auto& [track, index] = *found;
    const Access a(tx.document());
    const auto list = entries(a, track.sequence);
    const auto& self = list[index];
    if (self.transition)
    {
        return fail(Errc::unsupported, "transitions are trimmed by editing their length");
    }
    if (delta == 0)
    {
        return {};
    }
    const bool head = edge == Edge::head;
    constexpr auto kNone = std::numeric_limits<std::size_t>::max();
    std::size_t neighbourIndex = kNone;
    if (head && index > 0)
    {
        neighbourIndex = index - 1;
    }
    else if (!head && index + 1 < list.size())
    {
        neighbourIndex = index + 1;
    }
    if (neighbourIndex != kNone && list[neighbourIndex].transition)
    {
        return fail(Errc::unsupported, "remove the transition at this edit point first");
    }
    const auto length = head ? self.length - delta : self.length + delta;
    if (length < 1)
    {
        return fail(Errc::invalid_argument, "the segment would become empty");
    }
    if (auto r = setSegmentLength(tx, item, length); !r)
    {
        return r;
    }
    if (head && self.clip)
    {
        if (auto r = adjustStart(tx, item, delta); !r)
        {
            return r;
        }
    }
    if (!ripple && neighbourIndex != kNone)
    {
        const auto& other = list[neighbourIndex];
        const auto otherLength = head ? other.length + delta : other.length - delta;
        if (otherLength < (other.filler ? 0 : 1))
        {
            return fail(Errc::invalid_argument, "the neighbouring segment would become empty");
        }
        if (auto r = setSegmentLength(tx, other.id, otherLength); !r)
        {
            return r;
        }
        if (!head && other.clip)
        {
            if (auto r = adjustStart(tx, other.id, delta); !r)
            {
                return r;
            }
        }
        if (auto r = checkWithinSource(a, other.id); !r)
        {
            return r;
        }
    }
    if (auto r = checkWithinSource(a, item); !r)
    {
        return r;
    }
    return normalize(tx, track);
}

auto place(edit::Transaction& tx, ObjectId slot, std::int64_t position, ObjectId segment, bool insert) -> Result<void>
{
    auto track = openTrack(tx, slot);
    if (!track)
    {
        return propagate(track);
    }
    const Access a(tx.document());
    if (segment >= tx.document().objectCount() || tx.document().object(segment).parent != kNoObject || !a.isA(segment, "Segment"))
    {
        return fail(Errc::invalid_argument, "only a detached segment can be placed");
    }
    if (!sameDataDefinition(a, segment, track->sequence))
    {
        return fail(Errc::invalid_argument, "the segment's data kind does not match the track");
    }
    const auto length = lengthOf(a, segment);
    if (length < 1)
    {
        return fail(Errc::invalid_argument, "the segment has no length");
    }
    if (!insert)
    {
        for (const auto& e : entries(a, track->sequence))
        {
            if (e.transition && e.start < position + length && e.start + e.length > position)
            {
                return fail(Errc::invalid_argument, "the range overlaps a transition");
            }
        }
    }
    auto first = cutAt(tx, *track, position);
    if (!first)
    {
        return propagate(first);
    }
    if (!insert)
    {
        auto last = cutAt(tx, *track, position + length);
        if (!last)
        {
            return propagate(last);
        }
        const auto end = std::min(*last, components(a, track->sequence).size());
        for (auto i = end; i > *first; --i)
        {
            auto removed = removeAt(tx, *track, i - 1);
            if (!removed)
            {
                return propagate(removed);
            }
            if (auto r = edit::deleteObject(tx, *removed, true); !r && tx.document().object(*removed).parent != kNoObject)
            {
                return r;
            }
        }
    }
    if (auto r = insertAt(tx, *track, *first, segment); !r)
    {
        return r;
    }
    return normalize(tx, *track);
}

auto placeClip(edit::Transaction& tx, ObjectId slot, std::int64_t position, ObjectId sourceMob, std::uint32_t sourceSlot, std::int64_t sourceIn, std::int64_t length, bool insert) -> Result<ObjectId>
{
    auto track = openTrack(tx, slot);
    if (!track)
    {
        return propagate(track);
    }
    const Access a(tx.document());
    if (sourceMob >= tx.document().objectCount() || !a.isA(sourceMob, "Mob"))
    {
        return fail(Errc::invalid_argument, "the source is not a mob");
    }
    const auto slots = a.children(sourceMob, "Mob", "Slots");
    if (std::ranges::none_of(slots, [&](ObjectId s) -> bool { return a.integer(s, "MobSlot", "SlotID") == static_cast<std::int64_t>(sourceSlot); }))
    {
        return fail(Errc::invalid_argument, std::format("the source mob has no slot {}", sourceSlot));
    }
    const auto mobId = a.value(sourceMob, "Mob", "MobID");
    if (!mobId)
    {
        return fail(Errc::invalid_argument, "the source mob has no MobID");
    }
    auto clip = edit::createObject(tx, a.model_.findClassByName("SourceClip")->id);
    if (!clip)
    {
        return clip;
    }
    auto sourceIdPid = pidOf(a, "SourceReference", "SourceID");
    if (!sourceIdPid)
    {
        return propagate(sourceIdPid);
    }
    for (const auto& r : {
             copyDataDefinition(tx, track->sequence, *clip),
             setLength(tx, *clip, length),
             edit::setProperty(tx, *clip, *sourceIdPid, *mobId),
             setInt(tx, *clip, "SourceReference", "SourceMobSlotID", sourceSlot),
             setInt(tx, *clip, "SourceClip", "StartTime", sourceIn),
         })
    {
        if (!r)
        {
            return propagate(r);
        }
    }
    if (auto r = checkWithinSource(a, *clip); !r)
    {
        return propagate(r);
    }
    if (auto r = place(tx, slot, position, *clip, insert); !r)
    {
        return propagate(r);
    }
    return clip;
}

auto move(edit::Transaction& tx, ObjectId item, ObjectId toSlot, std::int64_t position, bool ripple) -> Result<void>
{
    auto found = locate(tx, item);
    if (!found)
    {
        return propagate(found);
    }
    auto [track, index] = *found;
    const Access a(tx.document());
    if (a.isA(item, "Transition"))
    {
        return fail(Errc::unsupported, "transitions cannot be moved");
    }
    if (ripple)
    {
        const auto list = entries(a, track.sequence);
        if (index + 1 < list.size() && list[index + 1].transition)
        {
            if (auto r = cutTransition(tx, track, index + 1); !r)
            {
                return r;
            }
        }
        if (index > 0 && list[index - 1].transition)
        {
            if (auto r = cutTransition(tx, track, index - 1); !r)
            {
                return r;
            }
            --index;
        }
        if (auto r = removeAt(tx, track, index); !r)
        {
            return propagate(r);
        }
    }
    else
    {
        auto filler = makeFiller(tx, track, lengthOf(a, item));
        if (!filler)
        {
            return propagate(filler);
        }
        if (auto r = removeAt(tx, track, index); !r)
        {
            return propagate(r);
        }
        if (auto r = insertAt(tx, track, index, *filler); !r)
        {
            return r;
        }
    }
    if (auto r = normalize(tx, track); !r)
    {
        return r;
    }
    return place(tx, toSlot, position, item, false);
}

auto addTrack(edit::Transaction& tx, ObjectId mob, TrackKind kind, const std::string& name) -> Result<ObjectId>
{
    const Access a(tx.document());
    if (mob >= tx.document().objectCount() || !a.isA(mob, "Mob"))
    {
        return fail(Errc::invalid_argument, "tracks can only be added to a mob");
    }
    if (kind != TrackKind::picture && kind != TrackKind::sound)
    {
        return fail(Errc::unsupported, "only picture and sound tracks can be added");
    }
    const Projector projector(tx.document());
    std::optional<ObjectId> definition;
    for (const auto s : a.children(mob, "Mob", "Slots"))
    {
        const auto segment = a.child(s, "MobSlot", "Segment");
        if (segment && projector.trackKindOf(*segment) == kind)
        {
            definition = a.weak(*segment, "Component", "DataDefinition");
            if (definition)
            {
                break;
            }
        }
    }
    if (!definition && kind == TrackKind::picture)
    {
        definition = findDataDefinition(a, { "01030202-0100-0000-060e-2b3404010101"_auid, "6f3c8ce1-6cef-11d2-807d-006008143e6f"_auid }, "Picture");
    }
    else if (!definition)
    {
        definition = findDataDefinition(a, { "01030202-0200-0000-060e-2b3404010101"_auid, "78e1ebe1-6cef-11d2-807d-006008143e6f"_auid }, "Sound");
    }
    if (!definition)
    {
        return fail(Errc::not_found, "the file has no suitable data definition");
    }
    std::uint32_t physical = 1;
    for (const auto s : a.children(mob, "Mob", "Slots"))
    {
        const auto segment = a.child(s, "MobSlot", "Segment");
        physical += segment && a.isA(s, "TimelineMobSlot") && projector.trackKindOf(*segment) == kind ? 1U : 0U;
    }
    const auto rate = firstTimelineRate(a, mob);
    const auto slotId = nextSlotId(a, mob);
    auto slot = edit::createObject(tx, a.model_.findClassByName("TimelineMobSlot")->id);
    auto sequence = edit::createObject(tx, a.model_.findClassByName("Sequence")->id);
    if (!slot || !sequence)
    {
        return fail(Errc::not_found, "the model lacks TimelineMobSlot or Sequence");
    }
    auto pid = [&](std::string_view cls, std::string_view prop) -> unsigned short { return pidOf(a, cls, prop).value_or(0); };
    std::vector<Result<void>> steps;
    steps.push_back(edit::setProperty(tx, *slot, pid("MobSlot", "SlotID"), Value(std::uint64_t{ slotId })));
    steps.push_back(edit::setProperty(tx, *slot, pid("TimelineMobSlot", "EditRate"), rateValue(rate)));
    steps.push_back(edit::setProperty(tx, *slot, pid("TimelineMobSlot", "Origin"), Value(std::int64_t{ 0 })));
    steps.push_back(edit::setProperty(tx, *slot, pid("MobSlot", "PhysicalTrackNumber"), Value(std::uint64_t{ physical })));
    if (!name.empty())
    {
        steps.push_back(edit::setProperty(tx, *slot, pid("MobSlot", "SlotName"), Value(name)));
    }
    steps.push_back(edit::setWeakRef(tx, *sequence, pid("Component", "DataDefinition"), *definition));
    steps.push_back(edit::ensureCollection(tx, *sequence, pid("Sequence", "Components")));
    steps.push_back(setLength(tx, *sequence, 0));
    steps.push_back(edit::setStrongRef(tx, *slot, pid("MobSlot", "Segment"), *sequence));
    steps.push_back(edit::insertIntoCollection(tx, mob, pid("Mob", "Slots"), a.children(mob, "Mob", "Slots").size(), *slot));
    for (const auto& r : steps)
    {
        if (!r)
        {
            return propagate(r);
        }
    }
    return slot;
}

auto removeTrack(edit::Transaction& tx, ObjectId slot) -> Result<void>
{
    const Access a(tx.document());
    if (slot >= tx.document().objectCount() || !a.isA(slot, "MobSlot"))
    {
        return fail(Errc::invalid_argument, "the object is not a track");
    }
    return edit::deleteObject(tx, slot);
}

auto addMarker(edit::Transaction& tx, ObjectId mob, std::int64_t position, const std::string& comment) -> Result<ObjectId>
{
    const Access a(tx.document());
    if (mob >= tx.document().objectCount() || !a.isA(mob, "Mob"))
    {
        return fail(Errc::invalid_argument, "markers can only be added to a mob");
    }
    auto pid = [&](std::string_view cls, std::string_view prop) -> unsigned short { return pidOf(a, cls, prop).value_or(0); };
    std::optional<ObjectId> sequence;
    for (const auto s : a.children(mob, "Mob", "Slots"))
    {
        const auto segment = a.child(s, "MobSlot", "Segment");
        if (a.isA(s, "EventMobSlot") && segment && a.isA(*segment, "Sequence"))
        {
            sequence = segment;
            break;
        }
    }
    if (!sequence)
    {
        const auto definition = findDataDefinition(a, { "01030201-1000-0000-060e-2b3404010101"_auid }, "Descriptive");
        std::uint64_t eventSlots = 0;
        for (const auto s : a.children(mob, "Mob", "Slots"))
        {
            eventSlots += a.isA(s, "EventMobSlot") ? 1U : 0U;
        }
        if (!definition)
        {
            return fail(Errc::not_found, "the file has no descriptive metadata data definition");
        }
        auto slot = edit::createObject(tx, a.model_.findClassByName("EventMobSlot")->id);
        auto created = edit::createObject(tx, a.model_.findClassByName("Sequence")->id);
        if (!slot || !created)
        {
            return fail(Errc::not_found, "the model lacks EventMobSlot or Sequence");
        }
        for (const auto& r : {
                 edit::setProperty(tx, *slot, pid("MobSlot", "SlotID"), Value(std::uint64_t{ nextSlotId(a, mob) })),
                 edit::setProperty(tx, *slot, pid("EventMobSlot", "EditRate"), rateValue(firstTimelineRate(a, mob))),
                 edit::setProperty(tx, *slot, pid("MobSlot", "PhysicalTrackNumber"), Value(std::uint64_t{ eventSlots + 1 })),
                 edit::setWeakRef(tx, *created, pid("Component", "DataDefinition"), *definition),
                 edit::ensureCollection(tx, *created, pid("Sequence", "Components")),
                 edit::setStrongRef(tx, *slot, pid("MobSlot", "Segment"), *created),
                 edit::insertIntoCollection(tx, mob, pid("Mob", "Slots"), a.children(mob, "Mob", "Slots").size(), *slot),
             })
        {
            if (!r)
            {
                return propagate(r);
            }
        }
        sequence = *created;
    }
    auto marker = edit::createObject(tx, a.model_.findClassByName("DescriptiveMarker")->id);
    if (!marker)
    {
        return marker;
    }
    std::optional<std::int64_t> described;
    const Projector projector(tx.document());
    for (const auto s : a.children(mob, "Mob", "Slots"))
    {
        const auto segment = a.child(s, "MobSlot", "Segment");
        if (a.isA(s, "TimelineMobSlot") && segment && (!described || projector.trackKindOf(*segment) == TrackKind::picture))
        {
            const bool picture = projector.trackKindOf(*segment) == TrackKind::picture;
            if (!described || picture)
            {
                described = a.integer(s, "MobSlot", "SlotID");
            }
            if (picture)
            {
                break;
            }
        }
    }
    Value::Array slots;
    if (described)
    {
        slots.emplace_back(static_cast<std::uint64_t>(*described));
    }
    for (const auto& r : {
             copyDataDefinition(tx, *sequence, *marker),
             edit::setProperty(tx, *marker, pid("Event", "Position"), Value(position)),
             edit::setProperty(tx, *marker, pid("Event", "Comment"), Value(comment)),
             edit::setProperty(tx, *marker, pid("DescriptiveMarker", "DescribedSlots"), Value(std::move(slots))),
         })
    {
        if (!r)
        {
            return propagate(r);
        }
    }
    const auto existing = components(a, *sequence);
    std::size_t index = 0;
    while (index < existing.size() && a.integer(existing[index], "Event", "Position").value_or(0) <= position)
    {
        ++index;
    }
    if (auto r = edit::insertIntoCollection(tx, *sequence, pid("Sequence", "Components"), index, *marker); !r)
    {
        return propagate(r);
    }
    return marker;
}

auto relink(edit::Transaction& tx, const std::string& find, const std::string& replace) -> Result<std::size_t>
{
    if (find.empty())
    {
        return fail(Errc::invalid_argument, "the text to find is empty");
    }
    const Access a(tx.document());
    auto pid = pidOf(a, "NetworkLocator", "URLString");
    if (!pid)
    {
        return propagate(pid);
    }
    std::size_t changed = 0;
    for (std::size_t i = 0; i < tx.document().objectCount(); ++i)
    {
        if (!a.isA(i, "NetworkLocator") || !tx.document().isAttached(i))
        {
            continue;
        }
        const auto original = a.string(i, "NetworkLocator", "URLString");
        auto url = original;
        for (auto pos = url.find(find); pos != std::string::npos; pos = url.find(find, pos + replace.size()))
        {
            url.replace(pos, find.size(), replace);
        }
        if (url != original)
        {
            if (auto r = edit::setProperty(tx, i, *pid, Value(url)); !r)
            {
                return propagate(r);
            }
            ++changed;
        }
    }
    return changed;
}

}
