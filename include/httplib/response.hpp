#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/form_data.hpp"
#include "httplib/html.hpp"
#include <boost/beast/http/message.hpp>
#include <filesystem>

namespace httplib
{

struct response : public http::response<body::any_body>
{
    using http::response<body::any_body>::message;

    response(http::response<body::any_body>&& other) { http::response<body::any_body>::operator=(std::move(other)); }

public:
    void set_empty_content(http::status status);
    void set_error_content(http::status status);

    void set_string_content(std::string_view data,
                            std::string_view content_type,
                            http::status status = http::status::ok);
    void set_string_content(std::string&& data, std::string_view content_type, http::status status = http::status::ok);
    void set_json_content(const body::json_body::value_type& data, http::status status = http::status::ok);
    void set_json_content(body::json_body::value_type&& data, http::status status = http::status::ok);

    void set_file_content(const fs::path& path, const http::fields& req_header = {});

    void set_form_data_content(const std::vector<form_data::field>& data);
};

} // namespace httplib