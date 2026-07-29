#include "httplib/body/buffer_body.hpp"

namespace httplib::body {

buffer_body::reader::reader(http::fields&, value_type& b)
    : body_(b)
{
}

void buffer_body::reader::init(boost::optional<std::uint64_t> const&,
                               boost::system::error_code& ec)
{
    ec = {};
}

std::size_t buffer_body::reader::put(const_buffers_type const& buffers,
                                     boost::system::error_code& ec)
{
    if (!body_.data) {
        BOOST_BEAST_ASSIGN_EC(ec, beast::http::error::need_buffer);
        return 0;
    }
    auto const bytes_transferred =
        net::buffer_copy(net::buffer(body_.data, body_.size), buffers);
    body_.data = static_cast<char*>(body_.data) + bytes_transferred;
    body_.size -= bytes_transferred;
    if (bytes_transferred == net::buffer_size(buffers))
        ec = {};
    else {
        BOOST_BEAST_ASSIGN_EC(ec, beast::http::error::need_buffer);
    }
    return bytes_transferred;
}

void buffer_body::reader::finish(boost::system::error_code& ec)
{
    ec = {};
}

buffer_body::writer::writer(http::fields const&, value_type const& b)
    : body_(b)
{
}

void buffer_body::writer::init(boost::system::error_code& ec)
{
    ec = {};
}

boost::optional<std::pair<buffer_body::writer::const_buffers_type, bool>>
buffer_body::writer::get(boost::system::error_code& ec)
{
    if (toggle_) {
        if (body_.more) {
            toggle_ = false;
            BOOST_BEAST_ASSIGN_EC(ec, beast::http::error::need_buffer);
        }
        else
            ec = {};
        return boost::none;
    }
    if (body_.data) {
        ec       = {};
        toggle_ = true;
        return {{const_buffers_type{body_.data, body_.size}, body_.more}};
    }
    if (body_.more) {
        BOOST_BEAST_ASSIGN_EC(ec, beast::http::error::need_buffer);
    }
    else
        ec = {};
    return boost::none;
}

} // namespace httplib::body
