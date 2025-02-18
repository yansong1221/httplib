#pragma once
#include "variant_message.hpp"

namespace httplib {

struct response : public http_response_variant {
    using http_response_variant::http_response_variant;

public:
    void set_string_content(std::string_view data, std::string_view content_type,
                            http::status status = http::status::ok) {
        base().set(http::field::content_type, content_type);
        change_body<http::string_body>() = data;
        base().result(status);
    }
    void set_file_content(const std::filesystem::path &path, beast::error_code &ec) {
        change_body<http::file_body>().open(path.string().c_str(), beast::file_mode::read, ec);
        if (ec)
            return;
        base().set(http::field::content_type, mime::get_mime_type(path.extension().string()));
        base().result(http::status::ok);
    }
};

} // namespace httplib