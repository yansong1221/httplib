#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/websocket_message.hpp"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

namespace httplib::server
{
    /**
     * @brief 服务器侧单个已建立的 WebSocket 连接句柄。
     *
     * 由服务器在 WebSocket 握手完成后创建，并以 @ref weak_ptr 交给用户
     * handler；持有 @c weak_ptr 可避免与连接生命周期形成循环引用，使用前需
     * 先 @c lock()。连接的所有操作都在其专属执行器上串行推进。
     *
     * 消息以 @ref httplib::websocket_message 表示，同时携带负载与文本/二进
     * 制帧类型；发送与接收均使用该类型。
     *
     * @par 线程安全
     * @li @ref send "send()" / @ref ping "ping()" / @ref close "close()" /
     *     @ref abort "abort()" 及其 @c async_ 版本会派发到连接的执行器上，
     *     可从任意线程调用。
     * @li @ref http_request "http_request()" 返回握手请求，可随时从任意线
     *     程读取。
     */
    class HTTPLIB_API websocket_conn : public std::enable_shared_from_this<websocket_conn>
    {
      public:
        /// 连接的生命周期弱引用；传给 handler，避免循环引用。
        using weak_ptr = std::weak_ptr<websocket_conn>;

        /// 连接建立（含握手失败）后调用的回调签名。
        using coro_open_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
        /// 连接关闭后调用的回调签名。
        using coro_close_handler_type = coro_open_handler_type;
        /// 每收到一条消息时调用的回调签名。
        using coro_message_handler_type
            = std::function<net::awaitable<void>(websocket_conn::weak_ptr, websocket_message)>;

      public:
        virtual ~websocket_conn() = default;

        /**
         * @brief 获取连接专属执行器。
         *
         * 连接的所有读写操作都在该执行器上串行推进；执行器由服务器执行器
         * 派生，可在其上调度连接级任务。
         *
         * @return 承载连接操作的执行器。
         */
        virtual net::any_io_executor get_executor() noexcept = 0;
        /**
         * @brief 优雅关闭连接，发送带原因文本的 close 帧。
         *
         * @param reason 关闭原因文本。
         *
         * @return 一个 future，在关闭完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        virtual std::future<boost::system::error_code> close(std::string_view reason) = 0;
        /**
         * @brief 优雅关闭连接，发送带原因文本的 close 帧。
         *
         * @param reason 关闭原因文本。
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        virtual net::awaitable<void> async_close(std::string_view reason, boost::system::error_code& ec) = 0;

        /**
         * @brief 立即中止连接。
         *
         * 直接关闭底层 socket，在途读写以 @c operation_aborted 完成，不发送
         * close 帧。
         *
         * @return 一个 future，在连接关闭后变为就绪。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        virtual std::future<void> abort() = 0;
        /**
         * @brief 立即中止连接，不阻塞。
         *
         * 等价于 @ref abort "abort()"，但返回可被 co_await 的 awaitable。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        virtual net::awaitable<void> async_abort() = 0;

        /**
         * @brief 获取本次连接握手时的 HTTP 请求。
         *
         * @return 指向握手请求的常量引用；可在整个连接生命周期内读取。
         */
        virtual request const& http_request() const = 0;
        /**
         * @brief 获取本次连接握手时的 HTTP 请求。
         *
         * @return 指向握手请求的引用；可写入 @c request::data 以携带连接级
         *         状态。
         */
        virtual request& http_request() = 0;

        /**
         * @brief 发送一条消息。
         *
         * @param msg 要发送的消息，携带负载与帧类型。
         *
         * @return 一个 future，在发送完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        virtual std::future<boost::system::error_code> send(websocket_message msg) = 0;
        /**
         * @brief 发送一条消息。
         *
         * @param msg 要发送的消息，携带负载与帧类型。
         * @param ec 输出参数，失败时写入错误码；连接未建立时写入
         *           @c not_connected。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        virtual net::awaitable<void> async_send(websocket_message const& msg, boost::system::error_code& ec) = 0;

        /**
         * @brief 发送 ping 控制帧。
         *
         * @param msg ping 负载。
         *
         * @return 一个 future，在发送完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        virtual std::future<boost::system::error_code> ping(std::string&& msg = std::string()) = 0;
        /**
         * @brief 发送 ping 控制帧。
         *
         * @param msg ping 负载。
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        virtual net::awaitable<void> async_ping(std::string_view msg, boost::system::error_code& ec) = 0;

        /**
         * @brief 优雅关闭连接，等价于原因文本为 @c "normal" 的
         *        @ref close(std::string_view) "close()"。
         *
         * @return 一个 future，在关闭完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        inline std::future<boost::system::error_code>
        close()
        {
            using namespace std::string_view_literals;
            return close("normal"sv);
        }
        /**
         * @brief 优雅关闭连接，等价于原因文本为 @c "normal" 的
         *        @ref async_close(std::string_view, boost::system::error_code&)
         *        "async_close()"。
         *
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        inline net::awaitable<void>
        async_close(boost::system::error_code& ec)
        {
            using namespace std::string_view_literals;
            co_return co_await async_close("normal"sv, ec);
        }
    };

} // namespace httplib::server
