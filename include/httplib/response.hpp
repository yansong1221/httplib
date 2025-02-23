#pragma once
#include "variant_message.hpp"

namespace httplib
{

struct response : public http::response<body::any_body>
{
    http::response<body::any_body>::message;

public:
    void set_string_content(std::string_view data,
                            std::string_view content_type,
                            http::status status = http::status::ok);
    void set_string_content(std::string&& data, std::string_view content_type, http::status status = http::status::ok);
    void set_json_content(const body::json_body::value_type& data, http::status status = http::status::ok);
    void set_json_content(body::json_body::value_type&& data, http::status status = http::status::ok);
    void set_file_content(const std::filesystem::path& path, beast::error_code& ec);
};

} // namespace httplib