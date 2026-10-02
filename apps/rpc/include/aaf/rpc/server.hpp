#pragma once

#include <aaf/edit/references.hpp>
#include <aaf/edit/session.hpp>
#include <aaf/rpc/json_value.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>

namespace aaf::rpc
{

/// JSON-RPC 2.0 server exposing an editing session to the UI (see SPEC §8.3).
/// Not thread-safe: the host calls it from a single worker thread.
class Server
{
public:
    using EventSink = std::function<void(const std::string& method, const Json& params)>;

    void setEventSink(EventSink sink) { sink_ = std::move(sink); }

    /// Handles one JSON-RPC request (or notification) and returns the response text (empty for notifications).
    [[nodiscard]] auto handle(std::string_view request) -> std::string;
    /// Calls a method directly.
    [[nodiscard]] auto call(const std::string& method, const Json& params) -> Result<Json>;

    [[nodiscard]] auto hasDocument() const noexcept -> bool { return session_.has_value(); }
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

private:
    [[nodiscard]] auto requireSession() -> Result<edit::Session*>;
    [[nodiscard]] auto info() const -> Json;
    [[nodiscard]] auto run(const std::string& description, const edit::Session::Command& command) -> Result<Json>;
    [[nodiscard]] auto timelineText(const Json& params) -> Result<std::string>;
    void emit(const std::string& method, const Json& params) const;
    void attachListener();
    /// True if `changes` may alter what `timeline.mobs` returns.
    [[nodiscard]] auto mobListChanged(const edit::ChangeSet& changes) -> bool;
    /// The reference index for the current document state, built on first use after a change.
    [[nodiscard]] auto references(const Document& document) -> const edit::ReferenceIndex&;

    std::optional<edit::Session> session_;
    std::filesystem::path path_;
    EventSink sink_;
    std::optional<std::set<MobId>> compositions_;
    std::optional<edit::ReferenceIndex> references_;
};

/// JSON form of a change set: `{"objects":[...], "created":[...], "properties":[{"object","pid"}], "referencedPropertiesChanged"}`.
[[nodiscard]] auto changeSetToJson(const edit::ChangeSet& changes) -> Json;
/// Short display label for an object: its Name, else its unique identifier, else its class name.
[[nodiscard]] auto labelOf(const Document& document, ObjectId id) -> std::string;

}
