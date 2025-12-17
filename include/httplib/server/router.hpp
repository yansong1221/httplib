#pragma once
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/websocket_conn.hpp"
#include <algorithm>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/fields.hpp>
#include <filesystem>
#include <list>
#include <string>
#include <string_view>

namespace httplib::server {

class request;
class response;

class router
{
public:
    virtual ~router() = default;

public:
    template<typename Func, typename... Aspects>
    void
    set_http_handler(http::verb method, std::string_view key, Func&& handler, Aspects&&... asps);

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key, Func&& handler, Aspects&&... asps)
    {
        static_assert(sizeof...(method) >= 1, "must set method");
        (set_http_handler(method, key, std::forward<Func>(handler), std::forward<Aspects>(asps)...),
         ...);
    }
    template<http::verb... method, typename Func, typename... Aspects>
        requires std::is_member_function_pointer_v<Func>
    void set_http_handler(std::string_view key,
                          Func&& handler,
                          util::class_type_t<Func>& owner,
                          Aspects&&... asps);

    template<typename Func, typename... Aspects>
    void set_http_not_found_handler(Func&& handler, Aspects&&... asps);


    template<typename OpenFunc, typename MessageFunc, typename CloseFunc>
    void set_ws_handler(std::string_view key,
                        OpenFunc&& open_handler,
                        MessageFunc&& message_handler,
                        CloseFunc&& close_handler);

    template<typename... Aspects>
    void set_static_mount_point(const std::string& mount_point,
                                const fs::path& dir,
                                Aspects&&... asps);
    template<typename... Aspects>
    void set_static_mount_point(mount_point_entry&& entry, Aspects&&... asps);

    template<typename Func>
    void set_http_post_handler(Func&& handler);

protected:
    using coro_http_handler_type =
        std::function<net::awaitable<void>(request& req, response& resp)>;
    using http_handler_type = std::function<void(request& req, response& resp)>;

    template<typename Func, typename... Aspects>
    coro_http_handler_type make_coro_http_handler(Func&& handler, Aspects&&... asps);


    virtual void set_http_handler_impl(http::verb method,
                                       std::string_view key,
                                       coro_http_handler_type&& handler)                      = 0;
    virtual void set_not_found_handler_impl(coro_http_handler_type&& handler)                 = 0;
    virtual void set_ws_handler_impl(std::string_view key,
                                     websocket_conn::coro_open_handler_type&& open_handler,
                                     websocket_conn::coro_message_handler_type&& message_handler,
                                     websocket_conn::coro_close_handler_type&& close_handler) = 0;
    virtual void set_http_post_handler_impl(coro_http_handler_type&& handler)                 = 0;
};

} // namespace httplib::server

#include "httplib/server/router.inl"