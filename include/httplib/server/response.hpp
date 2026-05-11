#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/server/helper.hpp"
#include "httplib/util/misc.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/json/value.hpp>
#include <filesystem>


namespace httplib::server {

class HTTPLIB_API response
{
public:
    class impl;
    // using http::response<body::any_body>::message;


    ~response();

    http::fields& base();
    const http::fields& base() const;

    void set(http::field name, std::string_view value);
    void set(std::string_view name, std::string_view value);

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

    template<typename Func>
    void set_stream_content(Func&& func,
                            std::string_view content_type,
                            http::status status = http::status::ok)
    {
        auto handler = util::make_coro_handler(std::move(func));
        set_stream_content_impl(std::move(handler), content_type, status);
    }

    impl* get_impl();
    const impl* get_impl() const;

protected:
    response(std::unique_ptr<impl>&& _impl);

private:
    using coro_stream_handler_type =
        std::function<net::awaitable<bool>(beast::flat_buffer& buffer, beast::error_code& ec)>;

    void set_stream_content_impl(coro_stream_handler_type&& handler,
                                 std::string_view content_type,
                                 http::status status = http::status::ok);

    std::unique_ptr<impl> impl_;
};

} // namespace httplib::server