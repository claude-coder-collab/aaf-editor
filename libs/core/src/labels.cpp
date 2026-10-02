#include "generated/smpte_labels_table.hpp"

#include <aaf/core/labels.hpp>

#include <algorithm>
#include <span>

namespace aaf
{

namespace
{

constexpr std::size_t kVersionByte = 15;

auto withoutVersion(const Auid& auid) noexcept -> Auid
{
    Auid out = auid;
    out.bytes[kVersionByte] = std::byte{ 0 };
    return out;
}

}

auto isSmpteUl(const Auid& auid) noexcept -> bool
{
    return auid.bytes[8] == std::byte{ 0x06 } && auid.bytes[9] == std::byte{ 0x0e } && auid.bytes[10] == std::byte{ 0x2b } && auid.bytes[11] == std::byte{ 0x34 };
}

auto findSmpteLabel(const Auid& auid) noexcept -> const SmpteLabel*
{
    if (!isSmpteUl(auid))
    {
        return nullptr;
    }
    const auto labels = detail::smpteLabels();
    const auto key = withoutVersion(auid);
    const auto it = std::ranges::lower_bound(labels, key, {}, [](const SmpteLabel& l) -> Auid { return withoutVersion(l.id); });
    return it != labels.end() && withoutVersion(it->id) == key ? &*it : nullptr;
}

auto smpteLabelFamily(const Auid& auid) -> std::vector<const SmpteLabel*>
{
    std::vector<const SmpteLabel*> out;
    if (!isSmpteUl(auid))
    {
        return out;
    }
    const auto key = withoutVersion(auid);
    const auto sameFamily = [&](const Auid& other) -> bool {
        const auto masked = withoutVersion(other);
        return std::ranges::equal(std::span(masked.bytes).first(4), std::span(key.bytes).first(4)) && std::ranges::equal(std::span(masked.bytes).subspan(8), std::span(key.bytes).subspan(8));
    };
    for (const auto& label : detail::smpteLabels())
    {
        if (!label.node && sameFamily(label.id))
        {
            out.push_back(&label);
        }
    }
    return out;
}

}
