#include "httplib/body/json_body.hpp"

#include <boost/json.hpp>
#include <boost/json/monotonic_resource.hpp>
#include <boost/json/serializer.hpp>
#include <boost/json/stream_parser.hpp>

namespace httplib::body {

struct json_body::writer::impl
{
    json::serializer serializer;
    char buffer[32768];

    explicit impl(value_type const& body)
    {
        serializer.reset(&body);
    }
};

json_body::writer::writer(const http::fields&, value_type const& body)
    : impl_(std::make_unique<impl>(body))
{
}

json_body::writer::~writer() = default;

json_body::writer::writer(writer&&) noexcept = default;
json_body::writer& json_body::writer::operator=(writer&&) noexcept = default;

void json_body::writer::init(boost::system::error_code& ec)
{
    ec = {};
}

boost::optional<std::pair<json_body::writer::const_buffers_type, bool>>
json_body::writer::get(boost::system::error_code& ec)
{
    ec = {};
    const auto len = impl_->serializer.read(impl_->buffer, sizeof(impl_->buffer));
    return std::make_pair(net::const_buffer(len.data(), len.size()),
                          !impl_->serializer.done());
}

struct json_body::reader::impl
{
    json::stream_parser parser;
    value_type& body;

    impl(const http::fields&, value_type& b)
        : body(b)
    {
    }
};

json_body::reader::reader(const http::fields& h, value_type& body)
    : impl_(std::make_unique<impl>(h, body))
{
}

json_body::reader::~reader() = default;

json_body::reader::reader(reader&&) noexcept = default;
json_body::reader& json_body::reader::operator=(reader&&) noexcept = default;

void json_body::reader::init(boost::optional<std::uint64_t> const& content_length,
                             boost::system::error_code& ec)
{
    if (content_length)
        impl_->parser.reset(
            json::make_shared_resource<json::monotonic_resource>(*content_length));
    ec = {};
}

std::size_t json_body::reader::put(net::const_buffer const& buffers, boost::system::error_code& ec)
{
    ec = {};
    return impl_->parser.write_some(
        static_cast<const char*>(buffers.data()), buffers.size(), ec);
}

void json_body::reader::finish(boost::system::error_code& ec)
{
    ec = {};
    if (impl_->parser.done())
        impl_->body = impl_->parser.release();
    else
        ec = boost::json::error::incomplete;
}

} // namespace httplib::body
