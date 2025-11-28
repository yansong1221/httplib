#include "response_impl.h"
#include "body/compressor.hpp"
#include "httplib/use_awaitable.hpp"
#include "mime_types.hpp"
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>

namespace httplib::server {

response_impl::response_impl(http_variant_stream_type& stream,
                             unsigned int version,
                             bool keep_alive)
    : stream_(stream)
{
    message_.result(http::status::not_found);
    message_.version(version);
    message_.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    message_.set(http::field::date, html::format_http_current_gmt_date());
    message_.keep_alive(keep_alive);
}

response_impl::response_impl(http_variant_stream_type& stream,
                             http::response<body::any_body>&& other)
    : stream_(stream)
    , message_(std::move(other))
{
}

bool response_impl::keep_alive() const
{
    return message_.keep_alive();
}

response::header_type& response_impl::header()
{
    return message_.base();
}

void response_impl::set_empty_content(http::status status)
{
    message_.result(status);
    message_.body() = body::empty_body::value_type {};
    message_.content_length(0);
}

void response_impl::set_error_content(http::status status)
{
    auto content = fmt::format(
        R"(<html>
<head><title>{0} {1}</title></head>
<body bgcolor="white">
<center><h1>{0} {1}</h1></center>
<hr><center>{2}</center>
</body>
</html>)",
        (int)status,
        http::obsolete_reason(status),
        message_.at(http::field::server));

    set_string_content(std::move(content), "text/html; charset=utf-8", status);
}

void response_impl::set_string_content(std::string&& data,
                                       std::string_view content_type,
                                       http::status status /*= http::status::ok*/)
{
    message_.content_length(data.size());
    message_.set(http::field::content_type, content_type);
    message_.result(status);
    message_.body() = std::move(data);
}

void response_impl::set_json_content(boost::json::value&& data,
                                     http::status status /*= http::status::ok*/)
{
    message_.result(status);
    message_.set(http::field::content_type, "application/json; charset=utf-8");
    message_.set(http::field::cache_control, "no-store");
    message_.body() = std::move(data);
}

void response_impl::set_file_content(const fs::path& path, const http::fields& req_header /*=*/)
{
    std::error_code ec;
    auto file_size = fs::file_size(path, ec);
    if (ec)
        return;
    auto file_write_time = html::file_last_write_time(path, ec);
    if (ec)
        return;

    bool is_valid = true;
    auto ranges   = html::parser_http_ranges(req_header[http::field::range], file_size, is_valid);
    if (!is_valid) {
        message_.set(http::field::content_range, fmt::format("bytes */{}", file_size));
        set_empty_content(http::status::range_not_satisfiable);
        return;
    }
    // etag
    auto file_etag_str = fmt::format("W/{}-{}", file_size, file_write_time);
    if (req_header[http::field::if_none_match] == file_etag_str) {
        set_empty_content(http::status::not_modified);
        return;
    }
    // last modified
    auto file_gmt_date_str = html::format_http_gmt_date(file_write_time);
    if (req_header[http::field::if_modified_since] == file_gmt_date_str) {
        set_empty_content(http::status::not_modified);
        return;
    }

    body::file_body::value_type file;
    file.open(path.string().c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open())
        return;

    file.content_type = mime::get_mime_type(path.extension().string());
    file.ranges       = ranges;

    message_.set(http::field::etag, file_etag_str);
    message_.set(http::field::last_modified, file_gmt_date_str);

    if (file.ranges.empty()) {
        message_.set(http::field::accept_ranges, "bytes");
        message_.set(http::field::content_type, file.content_type);
        // set(http::field::content_disposition,
        //     fmt::format("attachment;filename={}", (const
        //     char*)path.filename().u8string().c_str()));
        message_.result(http::status::ok);
        message_.content_length(file_size);
    }
    else if (file.ranges.size() == 1) {
        const auto& range = file.ranges.front();
        size_t part_size  = range.second + 1 - range.first;
        message_.set(http::field::content_range,
                     fmt::format("bytes {}-{}/{}", range.first, range.second, file_size));
        message_.set(http::field::content_type, file.content_type);
        message_.result(http::status::partial_content);
        message_.content_length(part_size);
    }
    else {
        file.boundary = html::generate_boundary();
        message_.set(http::field::content_type,
                     fmt::format("multipart/byteranges; boundary={}", file.boundary));
        message_.result(http::status::partial_content);
    }
    message_.body() = std::move(file);
}

void response_impl::set_form_data_content(const std::vector<form_data::field>& data)
{
    body::form_data_body::value_type value;
    value.boundary = html::generate_boundary();
    value.fields   = data;

    message_.result(http::status::ok);
    message_.set(http::field::content_type,
                 fmt::format("multipart/form-data; boundary={}", value.boundary));
    message_.body() = std::move(value);
}

void response_impl::set_redirect(std::string_view url,
                                 http::status status /*= http::status::moved_permanently*/)
{
    message_.set(http::field::location, url);
    set_empty_content(status);
}

httplib::net::awaitable<boost::system::error_code>
response_impl::reply(const std::chrono::steady_clock::duration& timeout,
                     const std::vector<std::string_view>& accept_encodings)
{
    if (!message_.has_content_length())
        message_.prepare_payload();

    for (const auto& encoding : accept_encodings) {
        if (body::compressor_factory::instance().is_supported_encoding(encoding)) {
            message_.set(http::field::content_encoding, encoding);
            message_.chunked(true);
            break;
        }
    }

    boost::system::error_code ec;

    http::response_serializer<body::any_body> serializer(message_);
    while (!serializer.is_done()) {
        stream_.expires_after(timeout);
        co_await http::async_write_some(stream_, serializer, net_awaitable[ec]);
        stream_.expires_never();
        if (ec)
            break;
    }
    co_return ec;
}

} // namespace httplib::server