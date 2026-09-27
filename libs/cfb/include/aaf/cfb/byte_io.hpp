#pragma once

#include <aaf/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace aaf::cfb
{

/// Random-access, read-only byte source. `read` must be safe to call concurrently.
class ByteSource
{
public:
    ByteSource() = default;
    ByteSource(const ByteSource&) = delete;
    ByteSource(ByteSource&&) = delete;
    auto operator=(const ByteSource&) -> ByteSource& = delete;
    auto operator=(ByteSource&&) -> ByteSource& = delete;
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual auto size() const noexcept -> std::uint64_t = 0;
    /// Reads up to `out.size()` bytes at `offset`; returns the number of bytes read (short only at end of source).
    [[nodiscard]] virtual auto read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t> = 0;
};

/// Byte source over an owned in-memory buffer.
class MemorySource final : public ByteSource
{
public:
    explicit MemorySource(std::vector<std::byte> data) :
        data_(std::move(data))
    {
    }
    [[nodiscard]] auto size() const noexcept -> std::uint64_t override { return data_.size(); }
    [[nodiscard]] auto read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t> override;
    [[nodiscard]] auto data() const noexcept -> std::span<const std::byte> { return data_; }

private:
    std::vector<std::byte> data_;
};

/// Byte source over a file on disk, using positional reads.
class FileSource final : public ByteSource
{
public:
    [[nodiscard]] static auto open(const std::filesystem::path& path) -> Result<std::unique_ptr<FileSource>>;
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource(FileSource&&) = delete;
    auto operator=(const FileSource&) -> FileSource& = delete;
    auto operator=(FileSource&&) -> FileSource& = delete;

    [[nodiscard]] auto size() const noexcept -> std::uint64_t override { return size_; }
    [[nodiscard]] auto read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t> override;

private:
    FileSource(std::FILE* file, std::uint64_t size) :
        file_(file),
        size_(size)
    {
    }
    std::FILE* file_;
    std::uint64_t size_;
    mutable std::mutex mutex_;
};

/// Sequential byte sink.
class ByteSink
{
public:
    ByteSink() = default;
    ByteSink(const ByteSink&) = delete;
    ByteSink(ByteSink&&) = delete;
    auto operator=(const ByteSink&) -> ByteSink& = delete;
    auto operator=(ByteSink&&) -> ByteSink& = delete;
    virtual ~ByteSink() = default;
    [[nodiscard]] virtual auto write(std::span<const std::byte> data) -> Result<void> = 0;
};

/// Byte sink appending to an in-memory buffer.
class MemorySink final : public ByteSink
{
public:
    [[nodiscard]] auto write(std::span<const std::byte> data) -> Result<void> override;
    [[nodiscard]] auto bytes() const noexcept -> const std::vector<std::byte>& { return data_; }
    [[nodiscard]] auto take() noexcept -> std::vector<std::byte> { return std::move(data_); }

private:
    std::vector<std::byte> data_;
};

/// Reads a whole file into memory.
[[nodiscard]] auto readFile(const std::filesystem::path& path) -> Result<std::vector<std::byte>>;

/// Writes a file atomically: `producer` writes to a temporary file next to `target`,
/// which is flushed to disk and then renamed over `target`. On failure `target` is untouched.
[[nodiscard]] auto writeFileAtomic(const std::filesystem::path& target, const std::function<Result<void>(ByteSink&)>& producer) -> Result<void>;

}
