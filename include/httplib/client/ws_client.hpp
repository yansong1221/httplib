#pragma once
#include "httplib/url/scheme.hpp"
#include "httplib/config.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/websocket_message.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

namespace httplib::client
{
    /**
     * @brief 基于 Boost.Asio 的异步 WebSocket 客户端。
     *
     * 客户端在独立的 strand 上串行推进连接、读写与关闭；不同客户端可并发，
     * 同一客户端内部的状态保持串行。
     *
     * 消息以 @ref httplib::websocket_message 表示，同时携带负载与文本/二进
     * 制帧类型；发送与接收均使用该类型。
     *
     * @par 线程安全
     * @li 连接、读写与关闭等成员内部会派发到客户端的 strand 上，可从任意线
     *     程调用。
     * @li @ref set_verify_ssl "set_verify_ssl()" / @ref set_ca_cert
     *     "set_ca_cert()" 使用原子量存储，可从任意线程调用，于下一次连接生
     *     效。
     * @li @ref logger "logger()" / @ref set_logger "set_logger()" 可随时从
     *     任意线程调用。
     */
    class HTTPLIB_API ws_client
    {
      public:
        /**
         * @brief 构造一个绑定到 I/O 执行上下文的 WebSocket 客户端。
         *
         * @param ex 客户端用于其异步操作的执行上下文。
         * @param host 目标主机，例如 @c "127.0.0.1"。
         * @param port 目标端口。
         * @param s 连接方案，@ref url::scheme::plain 或 @ref url::scheme::tls。
         */
        explicit ws_client(net::io_context& ex, std::string_view host, uint16_t port, url::scheme s = url::scheme::plain);
        /**
         * @brief 构造一个绑定到 I/O 执行器的 WebSocket 客户端。
         *
         * @param ex 客户端用于其异步操作的执行器。
         * @param host 目标主机，例如 @c "127.0.0.1"。
         * @param port 目标端口。
         * @param s 连接方案，@ref url::scheme::plain 或 @ref url::scheme::tls。
         */
        explicit ws_client(net::any_io_executor const& ex,
                           std::string_view host,
                           uint16_t port,
                           url::scheme s = url::scheme::plain);
        /**
         * @brief 销毁 WebSocket 客户端。
         *
         * 析构会中止底层连接；未完成的异步操作以 @c operation_aborted 完成。
         */
        ~ws_client();

      public:
        /**
         * @brief 发送一条消息。
         *
         * @param msg 要发送的消息，携带负载与帧类型。
         * @param ec 输出参数，失败时写入错误码；连接未建立时写入
         *           @c not_connected。
         *
         * @par 线程安全
         * 可从任意线程 co_await；内部按 strand 串行发送。
         */
        net::awaitable<void> async_send(websocket_message const& msg, boost::system::error_code& ec);
        /**
         * @brief 发送一条消息，不阻塞。
         *
         * 等价于 @ref async_send "async_send()"，但以 @c std::future 返回结
         * 果。
         *
         * @param msg 要发送的消息。
         *
         * @return 一个 future，在发送完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        std::future<boost::system::error_code> send(websocket_message msg);

        /**
         * @brief 建立连接并完成 WebSocket 握手。
         *
         * 依次执行名称解析、TCP/TLS 连接与 WebSocket 握手，整个过程受
         * @p timeout 限制；握手成功后超时被清除，不影响后续长连接。
         *
         * @param target 握手请求的 target（路径），例如 @c "/ws"。
         * @param headers 附加到握手请求的额外 header。
         * @param timeout 覆盖解析、连接与握手的总超时。
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        net::awaitable<void> async_connect(std::string_view target,
                                           http::fields const& headers,
                                           std::chrono::steady_clock::duration timeout,
                                           boost::system::error_code& ec);

        /**
         * @brief 读取下一条消息。
         *
         * @param msg 输出参数，负载会被替换为该消息的内容，帧类型写入
         *            @ref websocket_message::is_binary "is_binary()"。
         * @param ec 输出参数，失败时写入错误码；连接未建立或收到关闭帧时结
         *           束。
         *
         * @par 线程安全
         * 可从任意线程 co_await；@p msg 需活过本次 await。
         */
        net::awaitable<void> async_read(websocket_message& msg, boost::system::error_code& ec);

        /**
         * @brief 发送 ping 控制帧。
         *
         * @param msg ping 负载。
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        net::awaitable<void> async_ping(std::string_view msg, boost::system::error_code& ec);
        /**
         * @brief 发送 ping 控制帧，不阻塞。
         *
         * @param msg ping 负载。
         *
         * @return 一个 future，在发送完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        std::future<boost::system::error_code> ping(std::string&& msg = std::string());

        /**
         * @brief 发送 pong 控制帧。
         *
         * @param msg pong 负载。
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        net::awaitable<void> async_pong(std::string_view msg, boost::system::error_code& ec);
        /**
         * @brief 发送 pong 控制帧，不阻塞。
         *
         * @param msg pong 负载。
         *
         * @return 一个 future，在发送完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        std::future<boost::system::error_code> pong(std::string&& msg = std::string());

        /**
         * @brief 优雅关闭连接，发送 close 帧。
         *
         * @param ec 输出参数，失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        net::awaitable<void> async_close(boost::system::error_code& ec);
        /**
         * @brief 优雅关闭连接，不阻塞。
         *
         * @return 一个 future，在关闭完成后携带结果错误码。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        std::future<boost::system::error_code> close();

        /**
         * @brief 立即中止连接。
         *
         * 直接关闭底层 socket，在途读写以 @c operation_aborted 完成，不发送
         * close 帧。
         *
         * @par 线程安全
         * 可从任意线程 co_await。
         */
        net::awaitable<void> async_abort();
        /**
         * @brief 立即中止连接，不阻塞。
         *
         * 等价于 @ref async_abort "async_abort()"，返回一个 future。
         *
         * @par 线程安全
         * 可从任意线程调用。
         */
        std::future<void> abort();

        /**
         * @brief 判断连接是否已建立。
         *
         * @return 底层连接处于打开状态时为 @c true。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        bool is_open() const noexcept;

        /**
         * @brief 设置是否校验对端 TLS 证书。
         *
         * @param verify @c true 时执行证书校验（默认），@c false 时跳过。
         *
         * @par 线程安全
         * 可随时从任意线程调用；于下一次 @ref async_connect
         * "async_connect()" 生效。
         */
        void set_verify_ssl(bool verify);
        /**
         * @brief 设置用于校验对端证书的 CA 证书。
         *
         * @param cert PEM 编码的 CA 证书；为空时使用系统默认信任库。
         *
         * @par 线程安全
         * 可随时从任意线程调用；于下一次连接生效。
         */
        void set_ca_cert(std::string_view cert);

        /**
         * @brief 获取客户端使用的 logger。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        std::shared_ptr<spdlog::logger> logger() const;
        /**
         * @brief 替换客户端使用的 logger。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续日志输出立即生效。
         */
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        /**
         * @brief 建立连接并启动会话，不阻塞。
         *
         * 依次调用 @p open_handler；随后循环读取消息并调用
         * @p message_handler；连接结束后调用 @p close_handler。本成员立即返
         * 回，会话在客户端 strand 上推进。
         *
         * 三个回调均可以是返回 @c void 或 @c net::awaitable<void> 的可调用
         * 对象。
         *
         * @param target 握手请求的 target（路径）。
         * @param open_handler 连接建立（含失败）后调用，参数为结果错误码。
         * @param message_handler 每收到一条消息时调用，参数为
         *        @ref httplib::websocket_message。
         * @param close_handler 连接关闭后调用，无参数。
         * @param headers 附加到握手请求的额外 header。
         *
         * @par 线程安全
         * 可从任意线程调用。连接超时固定为 30 秒。
         */
        template <typename OpenFunc, typename MessageFunc, typename CloseFunc>
        void
        run(std::string_view target,
            OpenFunc&& open_handler,
            MessageFunc&& message_handler,
            CloseFunc&& close_handler,
            http::fields const& headers = {})
        {
            run_impl(target,
                     httplib::util::make_coro_handler(std::forward<OpenFunc>(open_handler)),
                     httplib::util::make_coro_handler(std::forward<MessageFunc>(message_handler)),
                     httplib::util::make_coro_handler(std::forward<CloseFunc>(close_handler)),
                     headers);
        }

        /**
         * @brief 建立连接并启动会话。
         *
         * 等价于 @ref run "run()"（不含 open 回调），但返回可 co_await 的
         * awaitable：连接完成后解析，携带结果错误码；随后的读循环在 strand
         * 上继续推进。
         *
         * @param target 握手请求的 target（路径）。
         * @param headers 附加到握手请求的额外 header。
         * @param message_handler 每收到一条消息时调用，参数为
         *        @ref httplib::websocket_message。
         * @param close_handler 连接关闭后调用，无参数。
         * @param ec 输出参数，连接失败时写入错误码。
         *
         * @par 线程安全
         * 可从任意线程 co_await。连接超时固定为 30 秒。
         */
        template <typename MessageFunc, typename CloseFunc>
        net::awaitable<void>
        async_run(std::string_view target,
                  http::fields const& headers,
                  MessageFunc&& message_handler,
                  CloseFunc&& close_handler,
                  boost::system::error_code& ec)
        {
            co_await async_run_impl(target,
                                    headers,
                                    httplib::util::make_coro_handler(std::forward<MessageFunc>(message_handler)),
                                    httplib::util::make_coro_handler(std::forward<CloseFunc>(close_handler)),
                                    ec);
        }

      private:
        using coro_open_handler_type = std::function<net::awaitable<void>(boost::system::error_code)>;
        using coro_close_handler_type = std::function<net::awaitable<void>()>;
        using coro_message_handler_type = std::function<net::awaitable<void>(websocket_message)>;

        net::awaitable<void> async_run_impl(std::string_view target,
                                            http::fields const& headers,
                                            coro_message_handler_type&& message_handler,
                                            coro_close_handler_type&& close_handler,
                                            boost::system::error_code& ec);
        void run_impl(std::string_view target,
                      coro_open_handler_type&& open_handler,
                      coro_message_handler_type&& message_handler,
                      coro_close_handler_type&& close_handler,
                      http::fields const& headers = {});

      private:
        ws_client(ws_client const&) = delete;
        ws_client& operator=(ws_client const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;
    };
} // namespace httplib::client
