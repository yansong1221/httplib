#include "httplib/server/response.hpp"
#include "mime_types.hpp"
#include <boost/beast/version.hpp>

namespace httplib::server {
response::response(unsigned int version, bool keep_alive)
{
    this->result(http::status::not_found);
    this->version(version);
    this->set(http::field::server, BOOST_BEAST_VERSION_STRING);
    this->set(http::field::date, html::format_http_current_gmt_date());
    this->keep_alive(keep_alive);
}
void response::set_empty_content(http::status status)
{
    this->result(status);
    this->body() = body::empty_body::value_type {};
    this->content_length(0);

    stream_handler_ = nullptr;
}

void response::set_error_content(http::status status)
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
        this->at(http::field::server));

    this->set_string_content(std::move(content), "text/html; charset=utf-8", status);
}

void response::set_string_content(std::string&& data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    this->content_length(data.size());
    this->set(http::field::content_type, content_type);
    this->result(status);
    this->body() = std::move(data);

    stream_handler_ = nullptr;
}

void response::set_json_content(boost::json::value&& data,
                                http::status status /*= http::status::ok*/)
{
    this->result(status);
    this->set(http::field::content_type, "application/json; charset=utf-8");
    this->set(http::field::cache_control, "no-store");
    this->body() = std::move(data);

    stream_handler_ = nullptr;
}

void response::set_file_content(const fs::path& path, const http::fields& req_header /*= {}*/)
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
        this->set(http::field::content_range, fmt::format("bytes */{}", file_size));
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

    this->set(http::field::etag, file_etag_str);
    this->set(http::field::last_modified, file_gmt_date_str);

    if (file.ranges.empty()) {
        this->set(http::field::accept_ranges, "bytes");
        this->set(http::field::content_type, file.content_type);
        // set(http::field::content_disposition,
        //     fmt::format("attachment;filename={}", (const
        //     char*)path.filename().u8string().c_str()));
        this->result(http::status::ok);
        this->content_length(file_size);
    }
    else if (file.ranges.size() == 1) {
        const auto& range = file.ranges.front();
        size_t part_size  = range.second + 1 - range.first;
        this->set(http::field::content_range,
                  fmt::format("bytes {}-{}/{}", range.first, range.second, file_size));
        this->set(http::field::content_type, file.content_type);
        this->result(http::status::partial_content);
        this->content_length(part_size);
    }
    else {
        file.boundary = html::generate_boundary();
        this->set(http::field::content_type,
                  fmt::format("multipart/byteranges; boundary={}", file.boundary));
        this->result(http::status::partial_content);
    }
    body() = std::move(file);

    stream_handler_ = nullptr;
}

void response::set_form_data_content(std::vector<form_data::field>&& data)
{
    body::form_data_body::value_type value;
    value.boundary = html::generate_boundary();
    value.fields   = std::move(data);

    this->result(http::status::ok);
    this->set(http::field::content_type,
              fmt::format("multipart/form-data; boundary={}", value.boundary));
    this->body() = std::move(value);
}

void response::set_redirect(std::string_view url,
                            http::status status /*= http::status::moved_permanently*/)
{
    this->set(http::field::location, url);
    set_empty_content(status);
}

void response::set_stream_content_impl(coro_stream_handler_type&& handler,
                                       std::string_view content_type,
                                       http::status status /*= http::status::ok*/)
{
    stream_handler_ = std::move(handler);
    this->set(http::field::content_type, content_type);
    this->result(status);
    this->body() = body::empty_body::value_type {};
}

} // namespace httplib::server