#pragma once

#include <aaf/core/document.hpp>
#include <aaf/core/writer.hpp>
#include <aaf/edit/transaction.hpp>

#include <functional>
#include <string>
#include <vector>

namespace aaf::edit
{

/// An editing session: a document with an undo/redo history.
/// Every change runs as a command in a transaction that is validated before it is committed;
/// a failing or invalid command leaves the document unchanged.
class Session
{
public:
    using Command = std::function<Result<void>(Transaction&)>;
    using Listener = std::function<void(const ChangeSet&)>;

    explicit Session(Document document);
    [[nodiscard]] static auto open(const std::filesystem::path& path) -> Result<Session>;

    [[nodiscard]] auto document() const noexcept -> const Document& { return document_; }

    /// Runs `command` as one undoable step.
    [[nodiscard]] auto execute(std::string description, const Command& command) -> Result<ChangeSet>;
    [[nodiscard]] auto canUndo() const noexcept -> bool { return position_ > 0; }
    [[nodiscard]] auto canRedo() const noexcept -> bool { return position_ < history_.size(); }
    [[nodiscard]] auto undo() -> Result<ChangeSet>;
    [[nodiscard]] auto redo() -> Result<ChangeSet>;
    /// Descriptions of all steps; the first `position()` are applied.
    [[nodiscard]] auto history() const -> std::vector<std::string>;
    [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }

    /// True if the document differs from the last save (or from the file as opened).
    [[nodiscard]] auto dirty() const noexcept -> bool { return position_ != savedPosition_; }
    /// Saves the document; the history is kept.
    [[nodiscard]] auto save(const std::filesystem::path& path, const WriteOptions& options = {}) -> Result<void>;

    void setListener(Listener listener) { listener_ = std::move(listener); }

private:
    struct Step
    {
        std::string description;
        Journal journal;
    };

    [[nodiscard]] auto checkCommitted(const Transaction& tx) const -> Result<void>;
    [[nodiscard]] auto danglingCount() const -> std::size_t;
    void notify(const ChangeSet& changes) const;

    Document document_;
    std::vector<Step> history_;
    std::size_t position_ = 0;
    std::size_t savedPosition_ = 0;
    std::size_t dangling_ = 0;
    Listener listener_;
};

}
