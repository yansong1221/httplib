#pragma once
#include "httplib/config.hpp"
#include "httplib/util/string.hpp"
#include <boost/asio/streambuf.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>

namespace httplib::body
{
namespace io = boost::iostreams;

class compressor
{
public:
    enum class type
    {
        deflate,
        gzip
    };
    enum class mode
    {
        encode,
        decode,
    };
    compressor(mode m, type t)
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
    net::const_buffer buffer() const { return buffer_.data(); }
    void write(const net::const_buffer& buffer, bool more)
    {
        io::write(stream_, (const char*)buffer.data(), buffer.size());
        if (!more)
        {
            io::close(stream_);
        }
    }
    void flush()
    {
        std::ostream os(&stream_);
        os.flush();
    }
    void consume() { buffer_.consume(buffer_.size()); }

public:
    static std::unique_ptr<compressor> create(compressor::mode m, const std::string_view encoding)
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

private:
    net::streambuf buffer_;
    io::filtering_ostreambuf stream_;
};
} // namespace httplib::body