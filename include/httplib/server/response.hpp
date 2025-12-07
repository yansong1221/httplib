#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include "httplib/server/helper.hpp"
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
    void set_form_data_content(std::vector<form_data::field>&& data);

    void set_redirect(std::string_view url, http::status status = http::status::moved_permanently);

    template<typename Func>
    void set_stream_content(Func&& func,
                            std::string_view content_type,
                            http::status status = http::status::ok)
    {
        auto handler = helper::make_coro_handler(std::move(func));
        set_stream_content_impl(std::move(handler), content_type, status);
    }

private:
    using coro_stream_handler_type =
        std::function<net::awaitable<bool>(beast::flat_buffer& buffer, beast::error_code& ec)>;

    void set_stream_content_impl(coro_stream_handler_type&& handler,
                                 std::string_view content_type,
                                 http::status status = http::status::ok);

    coro_stream_handler_type stream_handler_;

    friend class session;
};

} // namespace httplib::server