#pragma once
#include "httplib/http_handler.hpp"
#include "httplib/websocket_conn.hpp"
#include <algorithm>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/fields.hpp>
#include <filesystem>
#include <list>
#include <string>
#include <string_view>

namespace httplib {
class router_impl;
class router
{
public:
    explicit router();
    virtual ~router();

public:
    // eg: "GET hello/" as a key
    template<typename Func, typename... Aspects>
    void
    set_http_handler(http::verb method, std::string_view key, Func&& handler, Aspects&&... asps);

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key, Func&& handler, Aspects&&... asps)
    {
        static_assert(sizeof...(method) >= 1, "must set http_method");
        (set_http_handler(method, key, handler, std::forward<Aspects>(asps)...), ...);
    }
    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key,
                          Func&& handler,
                          util::class_type_t<Func>& owner,
                          Aspects&&... asps);

    template<typename Func, typename... Aspects>
    void set_default_handler(Func&& handler, Aspects&&... asps);

    template<typename Func, typename... Aspects>
    void set_file_request_handler(Func&& handler, Aspects&&... asps);

    bool set_mount_point(const std::string& mount_point,
                         const std::filesystem::path& dir,
                         const http::fields& headers = {});

    bool remove_mount_point(const std::string& mount_point);

    template<typename OpenFunc, typename MessageFunc, typename CloseFunc>
    void set_ws_handler(std::string_view key,
                        OpenFunc&& open_handler,
                        MessageFunc&& message_handler,
                        CloseFunc&& close_handler);

    bool has_handler(http::verb method, std::string_view target) const;

    net::awaitable<void> routing(request& req, response& resp);

    struct ws_handler_entry
    {
        websocket_conn::coro_open_handler_type open_handler;
        websocket_conn::coro_close_handler_type close_handler;
        websocket_conn::coro_message_handler_type message_handler;
    };
    std::optional<ws_handler_entry> find_ws_handler(request& req) const;

private:
    void set_http_handler_impl(http::verb method,
                               std::string_view key,
                               coro_http_handler_type&& handler);
    void set_default_handler_impl(coro_http_handler_type&& handler);
    void set_file_request_handler_impl(coro_http_handler_type&& handler);
    void set_ws_handler_impl(std::string_view key,
                             websocket_conn::coro_open_handler_type&& open_handler,
                             websocket_conn::coro_message_handler_type&& message_handler,
                             websocket_conn::coro_close_handler_type&& close_handler);

private:
    std::unique_ptr<router_impl> impl_;
};

} // namespace httplib

#include "httplib/router.inl"