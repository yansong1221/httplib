#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/streaming/buffer_body_writer.hpp"
#include "httplib/streaming/chunk_writer.hpp"
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/streaming/ndjson_writer.hpp"
#include "httplib/streaming/sse_writer.hpp"
#include "httplib/util/misc.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/json/value.hpp>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>


namespace httplib::server {

class HTTPLIB_API response
{
public:
    ~response();

    http::fields& base();
    const http::fields& base() const;

    std::string_view operator[](http::field name) const;
    std::string_view operator[](std::string_view name) const;
    std::string_view at(http::field name) const;
    std::string_view at(std::string_view name) const;

    void set(http::field name, std::string_view value);
    void set(std::string_view name, std::string_view value);

    bool has(http::field name) const;
    bool has(std::string_view name) const;
    void erase(http::field name);
    void erase(std::string_view name);

    http::status result() const;
    unsigned result_int() const;

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
    void set_form_data_content(std::vector<html::form_data::field>&& data);

    void set_redirect(std::string_view url, http::status status = http::status::moved_permanently);

    class impl;

    using chunked_write_handler_type = std::function<net::awaitable<void>(httplib::chunk_writer&)>;
    void set_chunked_write_handler(chunked_write_handler_type&& handler,
                                   std::string_view content_type,
                                   http::status status = http::status::ok);

    using buffer_body_write_handler_type =
        std::function<net::awaitable<void>(httplib::streaming::buffer_body_writer&)>;
    void set_buffer_body_write_handler(buffer_body_write_handler_type&& handler,
                                       const http::fields& headers,
                                       http::status status = http::status::ok);

    using sse_write_handler_type = std::function<net::awaitable<void>(httplib::sse_writer&)>;
    void set_sse_write_handler(sse_write_handler_type&& handler);

    using ndjson_write_handler_type = std::function<net::awaitable<void>(httplib::ndjson_writer&)>;
    void set_ndjson_write_handler(ndjson_write_handler_type&& handler);

    impl* get_impl();
    const impl* get_impl() const;

protected:
    response(std::unique_ptr<impl>&& _impl);

private:
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::server