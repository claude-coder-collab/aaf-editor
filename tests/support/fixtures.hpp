#pragma once

#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace aaf::test
{

[[nodiscard]] auto fixturesDir() -> std::filesystem::path;
/// All `.aaf` files below `fixturesDir() / sub`, sorted; empty if the directory does not exist.
[[nodiscard]] auto aafFilesIn(const std::filesystem::path& sub) -> std::vector<std::filesystem::path>;

[[nodiscard]] auto bytesOf(std::string_view text) -> std::vector<std::byte>;
[[nodiscard]] auto patternBytes(std::size_t size, std::uint32_t seed) -> std::vector<std::byte>;

[[nodiscard]] auto openMemory(std::vector<std::byte> data) -> Result<cfb::Container>;
[[nodiscard]] auto writeToMemory(const cfb::Builder& builder) -> Result<std::vector<std::byte>>;

/// Describes the first difference between the trees below two entries (names, types, CLSIDs,
/// state bits, timestamps and stream contents), or returns an empty string if they are equal.
[[nodiscard]] auto diffTrees(const cfb::Container& a, const cfb::Container& b) -> std::string;

}
