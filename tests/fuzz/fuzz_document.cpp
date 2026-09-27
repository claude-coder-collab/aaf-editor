#include <aaf/core/document.hpp>
#include <aaf/core/writer.hpp>

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
    auto doc = aaf::Document::load(std::move(*container));
    if (!doc)
    {
        return 0;
    }
    for (std::size_t i = 0; i < doc->objectCount(); ++i)
    {
        const auto& o = doc->object(i);
        for (const auto& p : o.properties)
        {
            if (std::holds_alternative<aaf::DataProperty>(p.payload))
            {
                if (auto v = doc->decode(o, p))
                {
                    (void) v->toString();
                }
            }
        }
    }
    (void) aaf::validate(*doc);
    aaf::cfb::MemorySink sink;
    if (aaf::write(*doc, sink))
    {
        auto reopened = aaf::cfb::Container::open(std::make_unique<aaf::cfb::MemorySource>(sink.take()));
        if (!reopened || !aaf::Document::load(std::move(*reopened)))
        {
            __builtin_trap();
        }
    }
    return 0;
}
