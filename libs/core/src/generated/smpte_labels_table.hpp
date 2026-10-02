#pragma once

#include <aaf/core/labels.hpp>

#include <span>

namespace aaf::detail
{

/// Every label of the SMPTE Labels register, sorted by stored AUID bytes with the UL version byte zeroed.
[[nodiscard]] auto smpteLabels() noexcept -> std::span<const SmpteLabel>;

}
