#include "httplib/response.hpp"

#include "mime_types.hpp"

namespace httplib
{

void response::set_string_content(std::string&& data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    change_body<body::string_body>() = std::move(data);
    base().set(http::field::content_type, content_type);
    base().result(status);
}

void response::set_string_content(std::string_view data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    change_body<body::string_body>() = data;
    base().set(http::field::content_type, content_type);
}

void response::set_json_content(body::json_body::value_type&& data, http::status status /*= http::status::ok*/)
{
    change_body<body::json_body>() = std::move(data);
    base().set(http::field::content_type, "application/json");
    base().result(status);
}

void response::set_json_content(const body::json_body::value_type& data, http::status status /*= http::status::ok*/)
{
    change_body<body::json_body>() = data;
    base().set(http::field::content_type, "application/json");
    base().result(status);
}

void response::set_file_content(const std::filesystem::path& path, beast::error_code& ec)
{
    change_body<body::file_body>().open(path.string().c_str(), beast::file_mode::read, ec);
    if (ec) return;
    base().set(http::field::content_type, mime::get_mime_type(path.extension().string()));
    base().result(http::status::ok);
}

} // namespace httplib