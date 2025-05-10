
#include "httplib/body/query_params_body.hpp"
namespace httplib::body {

query_params_body::writer::writer(const http::fields&, value_type const& body)
    : body_(body)
{
}

void query_params_body::writer::init(boost::system::error_code& ec)
{
    ec      = {};
    buffer_ = html::make_http_query_params(body_);
}

boost::optional<std::pair<query_params_body::writer::const_buffers_type, bool>>
query_params_body::writer::get(boost::system::error_code& ec)
{
    ec = {};
    return {{net::buffer(buffer_), false}};
}

query_params_body::reader::reader(const http::fields&, value_type& body)
    : body_(body)
{
}

void query_params_body::reader::init(boost::optional<std::uint64_t> const& content_length,
                                     boost::system::error_code& ec)
{
    if (content_length)
        buffer_.reserve(*content_length);
    ec = {};
}

std::size_t query_params_body::reader::put(net::const_buffer const& buffers,
                                           boost::system::error_code& ec)
{
    ec = {};
    buffer_.append((const char*)buffers.data(), buffers.size());
    return buffers.size();
}

void query_params_body::reader::finish(boost::system::error_code& ec)
{
    ec            = {};
    bool is_valid = true;
    body_         = html::parse_http_query_params(buffer_, is_valid);

    if (!is_valid) {
        ec = http::error::unexpected_body;
    }
}

} // namespace httplib::body