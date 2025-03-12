#include "httplib/body/json_body.hpp"

#include <boost/json.hpp>
#include <boost/json/monotonic_resource.hpp>

namespace httplib::body {

json_body::writer::writer(const http::fields&, value_type const& body)
{
    // The serializer holds a pointer to the value, so all we need to do is to reset it.
    serializer.reset(&body);
}

void
json_body::writer::init(boost::system::error_code& ec)
{
    // The serializer always works, so no error can occur here.
    ec = {};
}

boost::optional<std::pair<json_body::writer::const_buffers_type, bool>>
json_body::writer::get(boost::system::error_code& ec)
{
    ec = {};
    // We serialize as much as we can with the buffer. Often that'll suffice
    const auto len = serializer.read(buffer, sizeof(buffer));
    return std::make_pair(net::const_buffer(len.data(), len.size()), !serializer.done());
}

json_body::reader::reader(const http::fields&, value_type& body) : body(body) { }

void
json_body::reader::init(boost::optional<std::uint64_t> const& content_length,
                        boost::system::error_code& ec)
{
    // If we know the content-length, we can allocate a monotonic resource to increase the
    // parsing speed. We're using it rather then a static_resource, so a consumer can
    // modify the resulting value. It is also only assumption that the parsed json will be
    // smaller than the serialize one, it might not always be the case.
    if (content_length)
        parser.reset(
            json::make_shared_resource<json::monotonic_resource>(*content_length));
    ec = {};
}

std::size_t
json_body::reader::put(net::const_buffer const& buffers, boost::system::error_code& ec)
{
    ec = {};
    // The parser just uses the `ec` to indicate errors, so we don't need to do anything.
    return parser.write_some(
        static_cast<const char*>(buffers.data()), buffers.size(), ec);
}

void
json_body::reader::finish(boost::system::error_code& ec)
{
    ec = {};
    // We check manually if the json is complete.
    if (parser.done())
        body = parser.release();
    else
        ec = boost::json::error::incomplete;
}

} // namespace httplib::body