#pragma once

#include <aaf/cfb/builder.hpp>
#include <aaf/core/document.hpp>

#include <filesystem>
#include <optional>

namespace aaf
{

struct WriteOptions
{
    /// Keep the source file's storage names, local keys, index free-key ranges and byte orders.
    /// When false, names are regenerated as `<PropertyName>-<pid>` and collections are renumbered from 0.
    bool preserveLayout = true;
    /// Container version (sector size); defaults to the source file's.
    std::optional<cfb::Version> version;
};

/// Builds the compound-file tree for a document. Stream data is referenced from the document's
/// source container, which must outlive the returned builder's use.
[[nodiscard]] auto buildContainer(const Document& document, const WriteOptions& options = {}) -> Result<cfb::Builder>;

/// Serialises a document as an AAF file.
[[nodiscard]] auto write(const Document& document, cfb::ByteSink& sink, const WriteOptions& options = {}) -> Result<void>;

/// Saves a document to `path` atomically. `path` may be the document's own source file.
[[nodiscard]] auto save(const Document& document, const std::filesystem::path& path, const WriteOptions& options = {}) -> Result<void>;

/// Name generated for a property's storage or index when layout is not preserved.
[[nodiscard]] auto generatedStorageName(std::string_view propertyName, std::uint16_t pid) -> std::u16string;

}
