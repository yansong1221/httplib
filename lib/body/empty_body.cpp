#include "httplib/body/empty_body.hpp"

namespace httplib::body {
empty_body::writer::writer(const http::fields&, value_type const&) { }

void
empty_body::writer::init(beast::error_code& ec)
{
    ec = {};
}

boost::optional<std::pair<empty_body::writer::const_buffers_type, bool>>
empty_body::writer::get(beast::error_code& ec)
{
    ec = {};
    return boost::none;
}

empty_body::reader::reader(const http::fields&, value_type&) { }

std::size_t
empty_body::reader::put(net::const_buffer const&, beast::error_code& ec)
{
    ec = http::error::unexpected_body;
    return 0;
}

void
empty_body::reader::finish(beast::error_code& ec)
{
    ec = {};
}

void
empty_body::reader::init(boost::optional<std::uint64_t> const&, beast::error_code& ec)
{
    ec = {};
}


} // namespace httplib::body