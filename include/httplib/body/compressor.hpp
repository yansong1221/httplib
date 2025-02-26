#pragma once
#include "httplib/config.hpp"
#include <boost/asio/streambuf.hpp>
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
    explicit compressor(mode m, type t);

    net::const_buffer buffer() const;
    void write(const net::const_buffer& buffer, bool more);
    void finish();
    void consume();

public:
    static std::unique_ptr<compressor> create(compressor::mode m, const std::string_view encoding);

private:
    net::streambuf buffer_;
    io::filtering_ostreambuf stream_;
};
} // namespace httplib::body