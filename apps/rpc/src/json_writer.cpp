#include <aaf/rpc/json_writer.hpp>

namespace aaf::rpc
{

void JsonWriter::separate()
{
    if (!out_.empty() && out_.back() != '{' && out_.back() != '[' && out_.back() != ':')
    {
        out_ += ',';
    }
}

void JsonWriter::appendString(std::string_view text)
{
    static constexpr std::string_view kHex = "0123456789abcdef";
    out_ += '"';
    for (const char c : text)
    {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out_ += '\\';
            out_ += c;
        }
        else if (c == '\n')
        {
            out_ += "\\n";
        }
        else if (c == '\r')
        {
            out_ += "\\r";
        }
        else if (c == '\t')
        {
            out_ += "\\t";
        }
        else if (u < 0x20)
        {
            out_ += "\\u00";
            out_ += kHex[u >> 4U];
            out_ += kHex[u & 0xFU];
        }
        else
        {
            out_ += c;
        }
    }
    out_ += '"';
}

auto JsonWriter::beginObject() -> JsonWriter&
{
    separate();
    out_ += '{';
    return *this;
}

auto JsonWriter::endObject() -> JsonWriter&
{
    out_ += '}';
    return *this;
}

auto JsonWriter::beginArray() -> JsonWriter&
{
    separate();
    out_ += '[';
    return *this;
}

auto JsonWriter::endArray() -> JsonWriter&
{
    out_ += ']';
    return *this;
}

auto JsonWriter::key(std::string_view name) -> JsonWriter&
{
    separate();
    appendString(name);
    out_ += ':';
    return *this;
}

auto JsonWriter::value(std::string_view text) -> JsonWriter&
{
    separate();
    appendString(text);
    return *this;
}

auto JsonWriter::value(bool flag) -> JsonWriter&
{
    separate();
    out_ += flag ? "true" : "false";
    return *this;
}

auto JsonWriter::null() -> JsonWriter&
{
    separate();
    out_ += "null";
    return *this;
}

auto JsonWriter::raw(std::string_view json) -> JsonWriter&
{
    separate();
    out_ += json;
    return *this;
}

}
