#pragma once

#include <aaf/core/auid.hpp>

#include <string_view>
#include <vector>

namespace aaf
{

/// An entry of the SMPTE Labels register (ST 400), such as a compression scheme, essence container or
/// operational pattern.
struct SmpteLabel
{
    Auid id;
    std::string_view name;
    bool deprecated = false;
    /// True for a register node (a group of labels) rather than a leaf.
    bool node = false;
};

/// True if the AUID is a byte-swapped SMPTE Universal Label (its Data4 starts 06.0e.2b.34).
[[nodiscard]] auto isSmpteUl(const Auid& auid) noexcept -> bool;

/// The registered label with this identifier, ignoring the UL version byte, or nullptr.
[[nodiscard]] auto findSmpteLabel(const Auid& auid) noexcept -> const SmpteLabel*;

/// Leaf labels of the same family as `auid`: the same registry designator (ignoring the version byte) and
/// the same first four item bytes, for example every operational pattern or every essence container.
/// In register order; empty if `auid` is not a SMPTE UL.
[[nodiscard]] auto smpteLabelFamily(const Auid& auid) -> std::vector<const SmpteLabel*>;

}
