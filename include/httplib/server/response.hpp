#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/json/value.hpp>
#include <filesystem>


namespace httplib::server {

struct response : public http::response<body::any_body>
{
public:
    using http::response<body::any_body>::message;

    response(unsigned int version, bool keep_alive);
    ~response() = default;

    void set_empty_content(http::status status);
    void set_error_content(http::status status);

    void set_string_content(std::string_view data,
                            std::string_view content_type,
                            http::status status = http::status::ok)
    {
        set_string_content(std::string(data), content_type, status);
    }
    void set_string_content(std::string&& data,
                            std::string_view content_type,
                            http::status status = http::status::ok);

    void set_json_content(const boost::json::value& data, http::status status = http::status::ok)
    {
        set_json_content(boost::json::value(data), status);
    }
    void set_json_content(boost::json::value&& data, http::status status = http::status::ok);
    void set_file_content(const fs::path& path, const http::fields& req_header = {});
    void set_form_data_content(const std::vector<form_data::field>& data);

    void set_redirect(std::string_view url, http::status status = http::status::moved_permanently);
};

} // namespace httplib::server