#include "httplib/body/string_body.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/core/detail/clamp.hpp>
#include <boost/beast/core/detail/config.hpp>
#include <boost/beast/http/error.hpp>

namespace httplib::body {

string_body::reader::reader(const http::fields&, value_type& b) : body_(b) { }

void
string_body::reader::init(boost::optional<std::uint64_t> const& length,
                          beast::error_code& ec)
{
    if (length) {
        if (*length > body_.max_size()) {
            ec = http::error::buffer_overflow;
            return;
        }
        body_.reserve(beast::detail::clamp(*length));
    }
    ec = {};
}

std::size_t
string_body::reader::put(net::const_buffer const& buffers, beast::error_code& ec)
{
    auto const extra = net::buffer_size(buffers);
    auto const size  = body_.size();
    if (extra > body_.max_size() - size) {
        ec = http::error::buffer_overflow;
        return 0;
    }

    body_.resize(size + extra);
    ec         = {};
    char* dest = &body_[size];
    for (auto b : beast::buffers_range_ref(buffers)) {
        std::char_traits<char>::copy(dest, static_cast<char const*>(b.data()), b.size());
        dest += b.size();
    }
    return extra;
}

void
string_body::reader::finish(beast::error_code& ec)
{
    ec = {};
}

string_body::writer::writer(const http::fields&, value_type const& b) : body_(b) { }

boost::optional<std::pair<string_body::writer::const_buffers_type, bool>>
string_body::writer::get(beast::error_code& ec)
{
    ec = {};
    return {{const_buffers_type {body_.data(), body_.size()}, false}};
}

} // namespace httplib::body