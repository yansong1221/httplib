#pragma once
#include "httplib/headers.hpp"
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/server/websocket_conn.hpp"
#include "httplib/util/type_traits.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <list>
#include <string>
#include <string_view>

namespace httplib::server
{
    /**
     * @brief HTTP/WebSocket 路由表：把请求方法 + 路径映射到 handler，并承载
     *        全局与每路由中间件。
     *
     * @par 线程安全
     * 路由注册属于**配置阶段**，必须在 @ref http_server::run "run()" 之前由
     * 单一线程完成。内部**不做任何同步**：派发路径上的匹配与 handler 读取都是
     * 无锁的，服务运行期间调用下列任一方法属于未定义行为：
     *     - @ref use "use()"
     *     - @ref set_http_handler "set_http_handler()" / @ref
     *       set_lazy_http_handler "set_lazy_http_handler()"
     *     - @ref set_ws_handler "set_ws_handler()"
     *     - @ref set_http_not_found_handler "set_http_not_found_handler()" /
     *       @ref set_post_routing_handler "set_post_routing_handler()"
     *     - @ref set_static_mount_point "set_static_mount_point()" /
     *       @ref set_connect_handler "set_connect_handler()"
     *
     * @note 服务器停止时（@ref http_server::stop "stop()" 之后所有在途会话排空），
     *       框架会自动清空路由表。若同一个 @ref http_server 实例要再次
     *       @ref http_server::run "run()"，需在此之前重新注册路由。
     */
    class HTTPLIB_API router
    {
      public:
        virtual ~router() = default;

      public:
        template <typename... Aspects>
        void use(Aspects&&... asps);

        template <typename Func, typename... Aspects>
        void set_http_handler(httplib::method method,
                              std::string_view key,
                              Func&& handler,
                              Aspects&&... asps);

        template <httplib::method... method, typename Func, typename... Aspects>
        void
        set_http_handler(std::string_view key, Func handler, Aspects&&... asps)
        {
            static_assert(sizeof...(method) >= 1, "must set method");
            (set_http_handler(method, key, handler, std::forward<Aspects>(asps)...), ...);
        }
        template <httplib::method... method, typename Func, typename... Aspects>
            requires std::is_member_function_pointer_v<Func>
        void set_http_handler(std::string_view key, Func handler, util::class_type_t<Func>& owner, Aspects&&... asps);

        template <typename Func, typename... Aspects>
        void set_http_not_found_handler(Func&& handler, Aspects&&... asps);

        template <typename OpenFunc, typename MessageFunc, typename CloseFunc, typename... Aspects>
        void set_ws_handler(std::string_view key,
                            OpenFunc&& open_handler,
                            MessageFunc&& message_handler,
                            CloseFunc&& close_handler);

        template <typename... Aspects>
        void set_static_mount_point(std::string const& mount_point, fs::path const& dir, Aspects&&... asps);
        template <typename... Aspects>
        void set_static_mount_point(mount_point_entry&& entry, Aspects&&... asps);

        template <typename Func, typename... Aspects>
        void set_lazy_http_handler(httplib::method method, std::string_view key, Func&& handler, Aspects... asps);

        template <httplib::method... method, typename Func, typename... Aspects>
        void
        set_lazy_http_handler(std::string_view key, Func handler, Aspects&&... asps)
        {
            static_assert(sizeof...(method) >= 1, "must set method");
            (set_lazy_http_handler(method, key, handler, std::forward<Aspects>(asps)...), ...);
        }
        template <httplib::method... method, typename Func, typename... Aspects>
            requires std::is_member_function_pointer_v<Func>
        void set_lazy_http_handler(std::string_view key,
                                   Func handler,
                                   util::class_type_t<Func>& owner,
                                   Aspects&&... asps);

        template <typename Func>
        void set_post_routing_handler(Func&& handler);

        template <typename Func, typename... Aspects>
        void set_connect_handler(std::string_view key, Func&& handler, Aspects&&... asps);

      protected:
        using coro_http_handler_type = std::function<net::awaitable<void>(request& req, response& resp)>;
        using coro_mw_handler_type = std::function<net::awaitable<bool>(request& req, response& resp)>;
        using http_handler_type = std::function<void(request& req, response& resp)>;

        template <typename Func, typename... Aspects>
        coro_http_handler_type make_coro_http_handler(Func&& handler, Aspects&&... asps);

        virtual void set_http_handler_impl(httplib::method method,
                                           std::string_view key,
                                           coro_http_handler_type&& handler)
            = 0;
        virtual void set_not_found_handler_impl(coro_http_handler_type&& handler) = 0;
        virtual void set_ws_handler_impl(std::string_view key,
                                         websocket_conn::coro_open_handler_type&& open_handler,
                                         websocket_conn::coro_message_handler_type&& message_handler,
                                         websocket_conn::coro_close_handler_type&& close_handler)
            = 0;
        virtual void set_post_routing_handler_impl(coro_http_handler_type&& handler) = 0;

        virtual void set_lazy_http_handler_impl(httplib::method method,
                                                std::string_view key,
                                                coro_http_handler_type&& handler)
            = 0;

        virtual void use_impl(coro_mw_handler_type&& before, coro_mw_handler_type&& after) = 0;
    };

} // namespace httplib::server

#include "httplib/server/router.inl"