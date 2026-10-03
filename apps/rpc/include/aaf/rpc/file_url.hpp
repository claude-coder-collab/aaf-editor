#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aaf::rpc
{

/// A `file://` URL for an absolute local path: UTF-8, with every byte outside the unreserved set and `/` (and `:`
/// after a Windows drive letter) percent-encoded. Windows paths become `file:///C:/...`, UNC paths `file://host/...`.
[[nodiscard]] auto pathToFileUrl(const std::filesystem::path& path) -> std::string;

/// The local path of a `file:` URL (`file:///abs`, `file://localhost/abs`, `file:///C:/x`, or a UNC
/// `file://host/share/x`), percent-decoded as UTF-8; nullopt for other schemes or malformed escapes.
[[nodiscard]] auto fileUrlToPath(std::string_view url) -> std::optional<std::filesystem::path>;

}
