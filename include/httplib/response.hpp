#pragma once
#include "variant_message.hpp"

namespace httplib {

struct response : public http_response_variant {
    using http_response_variant::http_response_variant;

public:
    void set_string_content(std::string_view data, std::string_view content_type,
                            http::status status = http::status::ok) {
        change_body<body::string_body>() = data;
        base().set(http::field::content_type, content_type);
        base().result(status);
    }
    void set_string_content(std::string &&data, std::string_view content_type,
                            http::status status = http::status::ok) {      
        change_body<body::string_body>() = std::move(data);
        base().set(http::field::content_type, content_type);
        base().result(status);
    }
    void set_json_content(const body::json_body::value_type &data,
                          http::status status = http::status::ok) {
        change_body<body::json_body>() = data;
        base().set(http::field::content_type, "application/json");
        base().result(status);
    }
    void set_json_content(body::json_body::value_type &&data,
                          http::status status = http::status::ok) {
        change_body<body::json_body>() = std::move(data);
        base().set(http::field::content_type, "application/json");
        base().result(status);
    }
    void set_file_content(const std::filesystem::path &path, beast::error_code &ec) {
        change_body<body::file_body>().open(path.string().c_str(), beast::file_mode::read, ec);
        if (ec)
            return;
        base().set(http::field::content_type, mime::get_mime_type(path.extension().string()));
        base().result(http::status::ok);
    }
};

} // namespace httplib