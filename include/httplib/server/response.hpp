#pragma once
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/json/value.hpp>
#include <filesystem>


namespace httplib::server {

struct response
{
public:
    using header_type = http::response_header<http::fields>;

    virtual ~response() = default;

    virtual header_type& header()   = 0;
    virtual bool keep_alive() const = 0;

    virtual void set_empty_content(http::status status) = 0;
    virtual void set_error_content(http::status status) = 0;

    void set_string_content(std::string_view data,
                            std::string_view content_type,
                            http::status status = http::status::ok)
    {
        set_string_content(std::string(data), content_type, status);
    }
    virtual void set_string_content(std::string&& data,
                                    std::string_view content_type,
                                    http::status status = http::status::ok) = 0;

    void set_json_content(const boost::json::value& data, http::status status = http::status::ok)
    {
        set_json_content(boost::json::value(data), status);
    }
    virtual void set_json_content(boost::json::value&& data,
                                  http::status status = http::status::ok) = 0;

    virtual void set_file_content(const fs::path& path, const http::fields& req_header = {}) = 0;

    virtual void set_form_data_content(const std::vector<form_data::field>& data) = 0;

    virtual void set_redirect(std::string_view url,
                              http::status status = http::status::moved_permanently) = 0;
};

} // namespace httplib::server