#pragma once

#include <aaf/core/document.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aaf::preview
{

struct PreviewOptions
{
    /// Shown as the title.
    std::string fileName;
    /// Shown in the summary when known.
    std::optional<std::uintmax_t> fileSize;
    /// Rows in the clip list; the rest are counted.
    std::size_t maxClips = 1000;
    /// Rows in each mob list; the rest are counted.
    std::size_t maxMobs = 200;
};

/// A self-contained HTML page (no scripts, no external resources) summarising an AAF document: who wrote it,
/// object and mob counts, the main composition's timeline (zoomable with CSS only) and clip list, and the
/// other compositions, master mobs and source mobs. Light and dark appearance follow the system.
[[nodiscard]] auto renderPreview(const Document& document, const PreviewOptions& options) -> std::string;

/// A page that reports why a file could not be previewed.
[[nodiscard]] auto renderErrorPreview(std::string_view fileName, std::string_view message) -> std::string;

/// Opens the file and renders its preview, or an error page if it cannot be read or rendered.
[[nodiscard]] auto previewFile(const std::filesystem::path& path) -> std::string;

/// Formats a frame count as HH:MM:SS:FF, or HH:MM:SS;FF for drop-frame (SMPTE 12M; drop-frame applies to 30
/// and 60 fps only).
[[nodiscard]] auto formatTimecode(std::int64_t frames, std::uint32_t fps, bool drop) -> std::string;

/// Escapes text for HTML element content and attribute values.
[[nodiscard]] auto escapeHtml(std::string_view text) -> std::string;

}
