#pragma once

#include <aaf/core/document.hpp>
#include <aaf/timeline/rational.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aaf::timeline
{

enum class MobKind : std::uint8_t {
    composition,
    master,
    source,
    other,
};

enum class TrackKind : std::uint8_t {
    picture,
    sound,
    timecode,
    edgecode,
    descriptiveMetadata,
    data,
    other,
};

enum class SlotKind : std::uint8_t {
    timeline,
    event,
    fixed,
};

enum class ItemKind : std::uint8_t {
    sourceClip,
    filler,
    transition,
    operationGroup,
    essenceGroup,
    selector,
    nestedScope,
    scopeReference,
    pulldown,
    sequence,
    timecode,
    edgecode,
    marker,
    event,
    other,
};

[[nodiscard]] auto to_string(MobKind kind) noexcept -> std::string_view;
[[nodiscard]] auto to_string(TrackKind kind) noexcept -> std::string_view;
[[nodiscard]] auto to_string(SlotKind kind) noexcept -> std::string_view;
[[nodiscard]] auto to_string(ItemKind kind) noexcept -> std::string_view;
/// "Mono", "Stereo", "5.1", "7.1" or "N channels" for an audio channel count; empty for 0 (unknown).
[[nodiscard]] auto channelFormatName(std::uint32_t channels) -> std::string;

/// Where a source clip points.
struct SourceReference
{
    MobId mobId;
    std::uint32_t slotId = 0;
    std::int64_t startTime = 0;
    /// The referenced mob, if present in the file.
    std::optional<ObjectId> mob;
    std::string mobName;
    MobKind mobKind = MobKind::other;
    /// True if the clip is the end of a source chain (null MobID).
    bool original = false;

    auto operator==(const SourceReference&) const -> bool = default;
};

struct Timecode
{
    std::int64_t start = 0;
    std::uint32_t fps = 0;
    bool drop = false;

    auto operator==(const Timecode&) const -> bool = default;
};

/// An effect (OperationGroup) by object and operation definition name: a whole track's, or a clip's.
/// A track effect's group wraps the track's content.
struct Effect
{
    ObjectId object = kNoObject;
    std::string name;

    auto operator==(const Effect&) const -> bool = default;
};

struct Item
{
    ObjectId object = kNoObject;
    ItemKind kind = ItemKind::other;
    std::string className;
    /// Position in edit units of the track, relative to its origin. Transitions overlap the preceding segment.
    std::int64_t start = 0;
    std::int64_t length = 0;
    bool hasLength = true;
    std::string label;
    std::optional<SourceReference> source;
    /// Operation definition name for effects and transitions.
    std::string effect;
    std::optional<Timecode> timecode;
    /// Nested tracks: effect inputs, essence group choices, selector alternates, nested scope slots.
    std::vector<std::vector<Item>> nested;
    /// Marker comment for descriptive markers and comment markers.
    std::string comment;
    /// For an effect whose only input is a source clip, possibly through further single-input effects: that clip.
    /// For a multichannel clip: its channel combiner (the item itself when it is the combiner). The item then also
    /// carries the clip's `label` and `source`, so it can be shown as the clip.
    std::optional<ObjectId> clip;
    /// The effects around `clip`, from the track inwards.
    std::vector<Effect> effects;
    /// For a multichannel clip (an audio channel combiner, possibly inside effects): the source clip of each
    /// channel, in channel order. `label` and `source` then describe the clip as a whole.
    std::vector<ObjectId> channels;

    auto operator==(const Item&) const -> bool = default;
};

struct Track
{
    ObjectId slot = kNoObject;
    std::uint32_t slotId = 0;
    std::string name;
    std::optional<std::uint32_t> physicalNumber;
    TrackKind kind = TrackKind::other;
    SlotKind slotKind = SlotKind::timeline;
    Rational editRate{ 25, 1 };
    std::int64_t origin = 0;
    std::int64_t length = 0;
    /// The slot's segment. When `effects` is not empty, `items` are the content inside those effects.
    ObjectId segment = kNoObject;
    std::vector<Effect> effects;
    std::vector<Item> items;
    std::vector<std::string> warnings;
    /// Audio channels per clip: from the slot's `_TRACK_FORMAT` (stereo 2, 5.1 6, 7.1 8), else the most channels
    /// of any multichannel clip on the track; 0 when unknown (typically mono).
    std::uint32_t channels = 0;

    auto operator==(const Track&) const -> bool = default;
};

struct MobTimeline
{
    ObjectId mob = kNoObject;
    MobId mobId;
    std::string name;
    MobKind kind = MobKind::other;
    /// The projected tracks: every slot, or only the requested ones for a partial projection.
    std::vector<Track> tracks;
    /// Every slot of the mob, in order, whether projected or not.
    std::vector<ObjectId> slots;
    bool partial = false;
    /// Start timecode of the first timecode track, if any.
    std::optional<Timecode> timecode;
    /// The projected tracks' warnings, concatenated.
    std::vector<std::string> warnings;
};

struct MobSummary
{
    ObjectId object = kNoObject;
    MobId mobId;
    std::string name;
    MobKind kind = MobKind::other;
    std::size_t tracks = 0;
    /// True if the composition mob carries the "top level" usage code (or no usage code but no other mob references it).
    bool topLevel = false;
};

/// One step of a source chain.
struct ChainLink
{
    ObjectId mob = kNoObject;
    MobId mobId;
    std::string mobName;
    MobKind mobKind = MobKind::other;
    std::uint32_t slotId = 0;
    std::int64_t position = 0;
    Rational editRate;
    std::string descriptor;
};

struct EssenceInfo
{
    bool embedded = false;
    ObjectId essenceData = kNoObject;
    std::vector<std::string> locators;
    std::string descriptor;
};

enum class ChainStatus : std::uint8_t {
    resolved,
    missingMob,
    missingSlot,
    cycle,
};

[[nodiscard]] auto to_string(ChainStatus status) noexcept -> std::string_view;

struct SourceChain
{
    std::vector<ChainLink> links;
    ChainStatus status = ChainStatus::resolved;
    std::optional<EssenceInfo> essence;
};

/// Read-only view of the mobs and timelines of a document; rebuild after edits.
class Projector
{
public:
    explicit Projector(const Document& document);

    [[nodiscard]] auto mobs() const -> std::vector<MobSummary>;
    [[nodiscard]] auto project(ObjectId mob) const -> Result<MobTimeline>;
    /// Projects only the tracks of `slots` (which must belong to `mob`); the result is marked partial.
    [[nodiscard]] auto project(ObjectId mob, std::span<const ObjectId> slots) const -> Result<MobTimeline>;
    /// The slots of `mob` whose tracks may differ after `changed` objects changed. Returns nullopt when a changed
    /// object lies outside the mob (another mob, a definition, a locator), because any track may show data from it.
    /// Detached objects are ignored: whatever held them is itself among the changes.
    [[nodiscard]] auto affectedSlots(ObjectId mob, std::span<const ObjectId> changed) const -> std::optional<std::vector<ObjectId>>;
    /// Follows a source clip's reference through master and source mobs to the physical essence. Given an
    /// effect or a channel combiner, follows its first input down to a source clip (the first channel).
    [[nodiscard]] auto resolve(ObjectId target) const -> Result<SourceChain>;
    [[nodiscard]] auto findMob(const MobId& id) const -> std::optional<ObjectId>;
    [[nodiscard]] auto trackKindOf(ObjectId component) const -> TrackKind;

private:
    [[nodiscard]] auto mobKind(ObjectId mob) const -> MobKind;
    [[nodiscard]] auto projectMob(ObjectId mob, const std::vector<ObjectId>* only) const -> Result<MobTimeline>;
    [[nodiscard]] auto buildTrack(ObjectId slot) const -> Track;
    [[nodiscard]] auto buildItem(ObjectId component, std::int64_t start, int depth, std::vector<std::string>& warnings) const -> Item;
    [[nodiscard]] auto buildSequence(ObjectId segment, int depth, std::vector<std::string>& warnings) const -> std::vector<Item>;

    const Document& doc_;
    std::vector<std::pair<MobId, ObjectId>> mobIndex_;
    std::vector<std::pair<MobId, ObjectId>> essenceIndex_;
};

}
