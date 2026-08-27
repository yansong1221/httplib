#include "httplib/body/buffer_body.hpp"

#include <boost/asio/buffer.hpp>
#include <cstring>

namespace httplib::body
{

    buffer_body::reader::reader(http::fields const&, value_type& b) : body_(b) {}

    void
    buffer_body::reader::init(boost::optional<std::uint64_t> const&, beast::error_code& ec)
    {
        ec = {};
    }

    std::size_t
    buffer_body::reader::put(net::const_buffer const& buffers, beast::error_code& ec)
    {
        ec = {};
        auto const total = buffers.size();
        auto const n = total <= body_.size ? total : body_.size;
        if (n > 0)
        {
            std::memcpy(body_.data, buffers.data(), n);
            body_.data = static_cast<char*>(body_.data) + n;
            body_.size -= n;
        }
        if (n < total)
        {
            body_.pending.append(static_cast<char const*>(buffers.data()) + n, total - n);
        }
        return total;
    }

    void
    buffer_body::reader::finish(beast::error_code& ec)
    {
        ec = {};
    }

    buffer_body::writer::writer(http::fields const&, value_type const& b) : body_(b) {}

    boost::optional<std::pair<buffer_body::writer::const_buffers_type, bool>>
    buffer_body::writer::get(beast::error_code& ec)
    {
        if (done_)
        {
            ec = {};
            return boost::none;
        }
        done_ = true;
        ec = {};
        return {
            { const_buffers_type { body_.data, body_.size }, false }
        };
    }

} // namespace httplib::body
