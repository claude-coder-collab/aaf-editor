#pragma once

#include <aaf/edit/transaction.hpp>
#include <aaf/timeline/timeline.hpp>

#include <string>

namespace aaf::timeline::ops
{

/// Positions are in edit units of the track, measured from the start of its sequence (as projected).
/// Every operation edits the sequence inside the slot (unwrapping track-level effects; a slot holding a
/// single segment is first wrapped in a Sequence), then merges adjacent fillers, drops empty fillers and
/// recomputes the lengths of the sequence and of any track-level effects around it.

/// Splits the clip under `position` into two, adjusting the second part's StartTime, and returns the second part.
/// Filler is not split (it would be merged straight back).
[[nodiscard]] auto split(edit::Transaction& tx, ObjectId slot, std::int64_t position) -> Result<ObjectId>;

/// Replaces a segment with Filler of the same length (a transition is replaced by a cut at its midpoint).
[[nodiscard]] auto lift(edit::Transaction& tx, ObjectId item) -> Result<void>;

/// Removes a segment and closes the gap; transitions next to it become cuts.
[[nodiscard]] auto rippleDelete(edit::Transaction& tx, ObjectId item) -> Result<void>;

enum class Edge : std::uint8_t {
    head,
    tail,
};

/// Moves an edge of a segment by `delta` edit units. A roll trim moves the edit point with the neighbour;
/// a ripple trim changes only this segment and shifts everything after it.
[[nodiscard]] auto trim(edit::Transaction& tx, ObjectId item, Edge edge, std::int64_t delta, bool ripple) -> Result<void>;

/// Places a detached segment at `position`, replacing whatever it overlaps (overwrite) or pushing it later (insert).
[[nodiscard]] auto place(edit::Transaction& tx, ObjectId slot, std::int64_t position, ObjectId segment, bool insert) -> Result<void>;

/// Creates a SourceClip of `length` referencing `sourceMob`'s slot `sourceSlot` from `sourceIn`, and places it.
[[nodiscard]] auto placeClip(edit::Transaction& tx, ObjectId slot, std::int64_t position, ObjectId sourceMob, std::uint32_t sourceSlot, std::int64_t sourceIn, std::int64_t length, bool insert) -> Result<ObjectId>;

/// Moves a segment to `position` on `toSlot` (which may be its own slot), leaving Filler behind (or closing the gap with `ripple`).
[[nodiscard]] auto move(edit::Transaction& tx, ObjectId item, ObjectId toSlot, std::int64_t position, bool ripple) -> Result<void>;

/// Adds an empty picture or sound track to a mob and returns the new slot.
[[nodiscard]] auto addTrack(edit::Transaction& tx, ObjectId mob, TrackKind kind, const std::string& name) -> Result<ObjectId>;

/// Removes a track from its mob.
[[nodiscard]] auto removeTrack(edit::Transaction& tx, ObjectId slot) -> Result<void>;

/// Adds a DescriptiveMarker at `position` (in edit units of the mob's first timeline track), creating the marker track if
/// needed. `DescribedSlots` names the first picture track, as Avid does; other tools (OTIO) require it.
[[nodiscard]] auto addMarker(edit::Transaction& tx, ObjectId mob, std::int64_t position, const std::string& comment) -> Result<ObjectId>;

/// Replaces `find` with `replace` in every NetworkLocator URL; returns the number of URLs that changed.
[[nodiscard]] auto relink(edit::Transaction& tx, const std::string& find, const std::string& replace) -> Result<std::size_t>;

/// Copies an object and everything it strongly references (collections holding sets are not supported).
[[nodiscard]] auto deepCopy(edit::Transaction& tx, ObjectId id) -> Result<ObjectId>;

}
