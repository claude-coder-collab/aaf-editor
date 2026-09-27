#pragma once

#include <aaf/core/document.hpp>

#include <nlohmann/json.hpp>

#include <limits>
#include <string>

namespace aaftool
{

inline constexpr int kUnlimitedDepth = std::numeric_limits<int>::max();

/// Converts a decoded value to its canonical JSON form (see SPEC §8.1).
[[nodiscard]] auto toJson(const aaf::Value& value) -> nlohmann::ordered_json;
/// Converts an object and its strongly referenced descendants (to `maxDepth` levels) to JSON.
[[nodiscard]] auto toJson(const aaf::Document& doc, aaf::ObjectId id, int maxDepth = kUnlimitedDepth) -> nlohmann::ordered_json;
/// Renders an object tree as indented text.
[[nodiscard]] auto toText(const aaf::Document& doc, aaf::ObjectId id, int maxDepth = kUnlimitedDepth) -> std::string;

}
