#include "httplib/response.hpp"

#include "mime_types.hpp"
#include <fmt/format.h>

namespace httplib
{

void response::set_string_content(std::string&& data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    body() = std::move(data);
    set(http::field::content_type, content_type);
    result(status);
}

void response::set_string_content(std::string_view data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    body() = std::string(data);
    set(http::field::content_type, content_type);
    result(status);
}

void response::set_json_content(body::json_body::value_type&& data, http::status status /*= http::status::ok*/)
{
    body() = std::move(data);
    result(status);
}

void response::set_json_content(const body::json_body::value_type& data, http::status status /*= http::status::ok*/)
{
    body() = data;
    result(status);
}

void response::set_file_content(const std::filesystem::path& path)
{
    set_file_content(path, {});
}

void response::set_file_content(const std::filesystem::path& path, const http_ranges& ranges)
{
    body::file_body::value_type file;
    file.open(path.string().c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open()) return;

    auto file_size = file.file_size();

    file.content_type = mime::get_mime_type(path.extension().string());
    file.ranges = ranges;

    if (file.ranges.empty())
    {
        set(http::field::accept_ranges, "bytes");
        set(http::field::content_type, file.content_type);
        set(http::field::content_length, std::to_string(file_size));
        result(http::status::ok);
    }
    else if (ranges.size() == 1)
    {
        const auto& range = ranges.front();
        set(http::field::content_range, fmt::format("bytes {}-{}/{}", range.first, range.second, file_size));
        set(http::field::content_type, file.content_type);
        result(http::status::partial_content);
    }
    else
    {
        file.boundary = util::generate_boundary();
        set(http::field::content_type, fmt::format("multipart/byteranges; boundary={}", file.boundary));
        result(http::status::partial_content);
    }
    body() = std::move(file);
}

} // namespace httplib