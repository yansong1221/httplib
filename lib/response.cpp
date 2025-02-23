#include "httplib/response.hpp"

#include "mime_types.hpp"

namespace httplib
{

void response::set_string_content(std::string&& data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    body() = std::move(data);
    base().set(http::field::content_type, content_type);
    base().result(status);
}

void response::set_string_content(std::string_view data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    body() = std::string(data);
    base().set(http::field::content_type, content_type);
}

void response::set_json_content(body::json_body::value_type&& data, http::status status /*= http::status::ok*/)
{
    body() = std::move(data);
    base().set(http::field::content_type, "application/json");
    base().result(status);
}

void response::set_json_content(const body::json_body::value_type& data, http::status status /*= http::status::ok*/)
{
    body() = data;
    base().set(http::field::content_type, "application/json");
    base().result(status);
}

void response::set_file_content(const std::filesystem::path& path, beast::error_code& ec)
{
    body::file_body::value_type file;
    file.open(path.string().c_str(), beast::file_mode::read, ec);
    if (ec) return;

    body() = std::move(file);
    
    base().set(http::field::content_type, mime::get_mime_type(path.extension().string()));
    base().result(http::status::ok);
}

} // namespace httplib