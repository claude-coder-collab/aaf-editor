#include <aaf/cfb/builder.hpp>
#include <aaf/cfb/container.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) -> int
{
    std::vector<std::byte> bytes(size);
    if (size > 0)
    {
        std::memcpy(bytes.data(), data, size);
    }
    auto container = aaf::cfb::Container::open(std::make_unique<aaf::cfb::MemorySource>(std::move(bytes)));
    if (!container)
    {
        return 0;
    }
    for (const auto& entry : container->entries())
    {
        if (entry.isStream())
        {
            if (auto reader = container->openStream(entry.id))
            {
                (void) reader->readAll(std::uint64_t{ 1 } << 24);
            }
        }
    }
    if (auto builder = aaf::cfb::Builder::fromContainer(*container))
    {
        aaf::cfb::MemorySink sink;
        (void) aaf::cfb::write(*builder, sink);
    }
    return 0;
}
