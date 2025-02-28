#pragma once
#include "httplib/config.hpp"
#include <boost/asio/streambuf.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/zstd.hpp>
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
        gzip,
        zstd
    };
    enum class mode
    {
        encode,
        decode,
    };
    explicit compressor(mode m, type t)
    {
        if (m == mode::encode)
        {
            switch (t)
            {
                case type::gzip: stream_.push(io::gzip_compressor()); break;
                case type::deflate: stream_.push(io::zlib_compressor()); break;
                case type::zstd: stream_.push(io::zstd_compressor()); break;
            }
        }
        else if (m == mode::decode)
        {
            switch (t)
            {
                case type::gzip: stream_.push(io::gzip_decompressor()); break;
                case type::deflate: stream_.push(io::zlib_decompressor()); break;
                case type::zstd: stream_.push(io::zstd_decompressor()); break;
            }
        }
        stream_.push(buffer_);
    }

    net::const_buffer buffer() const { return buffer_.data(); }
    void write(const net::const_buffer& buffer, bool more = true)
    {
        io::write(stream_, (const char*)buffer.data(), buffer.size());
        if (!more)
        {
            io::close(stream_);
        }
    }
    void finish() { io::close(stream_); }
    void consume() { buffer_.consume(buffer_.size()); }
    void consume(std::size_t bytes) { buffer_.consume(bytes); }

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
        else if (encoding == "zstd")
        {
            return std::make_unique<compressor>(m, compressor::type::zstd);
        }
        return nullptr;
    }

private:
    net::streambuf buffer_;
    io::filtering_ostreambuf stream_;
};
} // namespace httplib::body