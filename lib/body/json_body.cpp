#include "body/json_body.hpp"

#include <boost/json.hpp>
#include <boost/json/monotonic_resource.hpp>

namespace httplib::body
{
    json_body::writer::writer(http::fields const&, value_type const& body) { serializer_.reset(&body); }

    void
    json_body::writer::init(boost::system::error_code& ec)
    {
        ec = {};
    }

    boost::optional<std::pair<json_body::writer::const_buffers_type, bool>>
    json_body::writer::get(boost::system::error_code& ec)
    {
        ec = {};
        auto const len = serializer_.read(buffer_, sizeof(buffer_));
        return std::make_pair(net::const_buffer(len.data(), len.size()), !serializer_.done());
    }

    json_body::reader::reader(http::fields const&, value_type& body) : body_(body) {}

    void
    json_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
    {
        if (content_length)
        {
            static constexpr std::uint64_t max_json_size = 10 * 1024 * 1024;
            auto alloc_sz = std::min(*content_length, max_json_size);
            parser_.reset(json::make_shared_resource<json::monotonic_resource>(alloc_sz));
        }
        ec = {};
    }

    std::size_t
    json_body::reader::put(net::const_buffer const& buffers, boost::system::error_code& ec)
    {
        ec = {};
        return parser_.write_some(static_cast<char const*>(buffers.data()), buffers.size(), ec);
    }

    void
    json_body::reader::finish(boost::system::error_code& ec)
    {
        ec = {};
        if (parser_.done())
        {
            body_ = parser_.release();
        }
        else
        {
            ec = boost::json::error::incomplete;
        }
    }

} // namespace httplib::body
