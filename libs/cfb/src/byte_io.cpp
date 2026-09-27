#include <aaf/cfb/byte_io.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <random>
#include <system_error>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <io.h>
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace aaf::cfb
{

namespace
{

auto errnoMessage(int err) -> std::string
{
    return std::error_code(err, std::generic_category()).message();
}

auto seek(std::FILE* file, std::uint64_t offset) -> bool
{
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

auto openFile(const std::filesystem::path& path, bool write) -> std::FILE*
{
#ifdef _WIN32
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), write ? L"wb" : L"rb") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

auto syncFile(std::FILE* file) -> bool
{
    if (std::fflush(file) != 0)
    {
        return false;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

auto replaceFile(const std::filesystem::path& from, const std::filesystem::path& to) -> Result<void>
{
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        return fail(Errc::io, std::format("cannot replace {} (Windows error {})", to.string(), GetLastError()));
    }
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
    {
        return fail(Errc::io, std::format("cannot replace {}: {}", to.string(), ec.message()));
    }
#endif
    return {};
}

class FileSink final : public ByteSink
{
public:
    explicit FileSink(std::FILE* file) :
        file_(file)
    {
    }
    ~FileSink() override
    {
        if (file_ != nullptr)
        {
            (void) std::fclose(file_);
        }
    }
    FileSink(const FileSink&) = delete;
    FileSink(FileSink&&) = delete;
    auto operator=(const FileSink&) -> FileSink& = delete;
    auto operator=(FileSink&&) -> FileSink& = delete;

    [[nodiscard]] auto write(std::span<const std::byte> data) -> Result<void> override
    {
        if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file_) != data.size())
        {
            return fail(Errc::io, std::format("write failed: {}", errnoMessage(errno)));
        }
        return {};
    }

    [[nodiscard]] auto finish() -> Result<void>
    {
        const bool synced = syncFile(file_);
        const bool closed = std::fclose(file_) == 0;
        file_ = nullptr;
        if (!synced || !closed)
        {
            return fail(Errc::io, std::format("flush failed: {}", errnoMessage(errno)));
        }
        return {};
    }

private:
    std::FILE* file_;
};

}

auto MemorySource::read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t>
{
    if (offset >= data_.size())
    {
        return 0;
    }
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), data_.size() - offset));
    std::memcpy(out.data(), data_.data() + offset, n);
    return n;
}

auto FileSource::open(const std::filesystem::path& path) -> Result<std::unique_ptr<FileSource>>
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return fail(Errc::io, std::format("cannot stat {}: {}", path.string(), ec.message()));
    }
    std::FILE* file = openFile(path, false);
    if (file == nullptr)
    {
        return fail(Errc::io, std::format("cannot open {}: {}", path.string(), errnoMessage(errno)));
    }
    return std::unique_ptr<FileSource>(new FileSource(file, size));
}

FileSource::~FileSource()
{
    (void) std::fclose(file_);
}

auto FileSource::read(std::uint64_t offset, std::span<std::byte> out) const -> Result<std::size_t>
{
    if (offset >= size_)
    {
        return 0;
    }
    const std::scoped_lock lock(mutex_);
    if (!seek(file_, offset))
    {
        return fail(Errc::io, "seek failed", offset);
    }
    const auto n = std::fread(out.data(), 1, out.size(), file_);
    if (n < out.size() && std::ferror(file_) != 0)
    {
        std::clearerr(file_);
        return fail(Errc::io, "read failed", offset);
    }
    return n;
}

auto MemorySink::write(std::span<const std::byte> data) -> Result<void>
{
    data_.insert(data_.end(), data.begin(), data.end());
    return {};
}

auto readFile(const std::filesystem::path& path) -> Result<std::vector<std::byte>>
{
    auto source = FileSource::open(path);
    if (!source)
    {
        return std::unexpected(source.error());
    }
    std::vector<std::byte> data(static_cast<std::size_t>((*source)->size()));
    auto n = (*source)->read(0, data);
    if (!n)
    {
        return std::unexpected(n.error());
    }
    data.resize(*n);
    return data;
}

auto writeFileAtomic(const std::filesystem::path& target, const std::function<Result<void>(ByteSink&)>& producer) -> Result<void>
{
    std::random_device rd;
    auto temp = target;
    temp += std::format(".tmp-{:08x}", rd());

    std::FILE* file = openFile(temp, true);
    if (file == nullptr)
    {
        return fail(Errc::io, std::format("cannot create {}: {}", temp.string(), errnoMessage(errno)));
    }
    auto discard = [&temp](Error error) -> Result<void> {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return std::unexpected(std::move(error));
    };

    FileSink sink(file);
    if (auto r = producer(sink); !r)
    {
        return discard(r.error());
    }
    if (auto r = sink.finish(); !r)
    {
        return discard(r.error());
    }
    if (auto r = replaceFile(temp, target); !r)
    {
        return discard(r.error());
    }
    return {};
}

}
