#include "httplib/body/compressor.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>

namespace httplib::body
{

compressor::compressor(mode m, type t)
{
    if (m == mode::encode)
    {
        switch (t)
        {
            case type::gzip: stream_.push(io::gzip_compressor()); break;
            case type::deflate: stream_.push(io::zlib_compressor()); break;
        }
    }
    else if (m == mode::decode)
    {
        switch (t)
        {
            case type::gzip: stream_.push(io::gzip_decompressor()); break;
            case type::deflate: stream_.push(io::zlib_decompressor()); break;
        }
    }
    stream_.push(buffer_);
}

void compressor::write(const net::const_buffer& buffer, bool more)
{
    io::write(stream_, (const char*)buffer.data(), buffer.size());
    if (!more)
    {
        io::close(stream_);
    }
}

void compressor::finish() { io::close(stream_); }

void compressor::consume() { buffer_.consume(buffer_.size()); }

std::unique_ptr<compressor> compressor::create(compressor::mode m, const std::string_view encoding)
{
    if (encoding == "gzip")
    {
        return std::make_unique<compressor>(m, compressor::type::gzip);
    }
    else if (encoding == "deflate")
    {
        return std::make_unique<compressor>(m, compressor::type::deflate);
    }
    return nullptr;
}

httplib::net::const_buffer compressor::buffer() const { return buffer_.data(); }

} // namespace httplib::body