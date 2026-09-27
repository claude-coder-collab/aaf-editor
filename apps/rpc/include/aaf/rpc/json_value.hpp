#pragma once

#include <aaf/core/metamodel.hpp>
#include <aaf/core/value.hpp>

#include <nlohmann/json.hpp>

#include <set>

namespace aaf::rpc
{

using Json = nlohmann::json;

/// Encodes a value in the tagged RPC form, e.g. `{"t":"int","v":"-5"}` (64-bit integers are strings).
[[nodiscard]] auto toJson(const Value& value) -> Json;
/// Decodes the tagged RPC form.
[[nodiscard]] auto valueFromJson(const Json& json) -> Result<Value>;

/// Describes a type for the UI's value editors.
[[nodiscard]] auto typeToJson(const MetaModel& model, const TypeDef& type) -> Json;
/// Adds `typeId` and every type it references to `out` (keyed by AUID string).
void collectTypes(const MetaModel& model, const Auid& typeId, Json& out, int depth = 0);

}
