#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/form_data.hpp"
#include "httplib/html.hpp"
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/message_generator.hpp>
#include <filesystem>

namespace httplib {

struct response
{
    response(unsigned int version, bool keep_alive);
    response(http::response<body::any_body>&& other);

public:
    http::response_header<>& header() { return message_; }
    bool keep_alive() const;

    void set_empty_content(http::status status);
    void set_error_content(http::status status);

    void set_string_content(std::string_view data,
                            std::string_view content_type,
                            http::status status = http::status::ok);
    void set_string_content(std::string&& data,
                            std::string_view content_type,
                            http::status status = http::status::ok);
    void set_json_content(const body::json_body::value_type& data,
                          http::status status = http::status::ok);
    void set_json_content(body::json_body::value_type&& data,
                          http::status status = http::status::ok);

    void set_file_content(const fs::path& path, const http::fields& req_header = {});

    void set_form_data_content(const std::vector<form_data::field>& data);

    void set_redirect(std::string_view url, http::status status = http::status::moved_permanently);

    http::response<body::any_body>& message() { return message_; }
    http::message_generator to_message() { return std::move(message_); }

    void prepare_payload();

private:
    http::response<body::any_body> message_;
};

} // namespace httplib