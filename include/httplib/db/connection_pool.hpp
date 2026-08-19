#pragma once

#include "config.hpp"
#include "fwd.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace httplib::db
{

    /**
     * \brief 连接池参数（backend 无关）。
     */
    struct pool_params
    {
        size_t min_connections = 2;
        size_t max_connections = 16;
        // 周期/超时期统一用 steady_clock::duration（与 client::http_client_pool 一致，支持亚秒级）；0 表示禁用。
        std::chrono::steady_clock::duration idle_timeout { std::chrono::seconds(300) };
        std::chrono::steady_clock::duration idle_check_interval { std::chrono::seconds(60) };
        std::chrono::steady_clock::duration health_check_interval { std::chrono::seconds(30) };
        bool validate_on_borrow = false;
    };

    /**
     * \brief 连接池。
     * \details
     * 维护一批可复用的连接，通过 \ref async_acquire 借出、句柄析构时归还。
     * 池本身不关心后端：由调用方提供 \c connect 工厂按需创建会话，
     * 池只负责生命周期、健康检查与等待唤醒。
     */
    class HTTPLIB_API connection_pool
    {
      private:
        struct impl;

      public:
        /**
         * \brief 借出的连接句柄。
         * \details 析构时把连接归还给池；同一时刻只能有一个持有者。
         */
        class HTTPLIB_API session_handle
        {
            friend class connection_pool;

            std::weak_ptr<impl> pool_;
            std::unique_ptr<session> sess_;

            session_handle(std::weak_ptr<impl> pool, std::unique_ptr<session> sess);

          public:
            session_handle();
            ~session_handle();

            session_handle(session_handle const&) = delete;
            session_handle& operator=(session_handle const&) = delete;

            session_handle(session_handle&& other) noexcept;
            session_handle& operator=(session_handle&& other) noexcept;

            session* get();
            session const* get() const;

            session* operator->();
            session const* operator->() const;

            session& operator*();
            session const& operator*() const;

            /// 提前归还连接。
            void release();
        };

      public:
        /// 创建一条新连接的工厂；参数为池所属 executor。
        using connect_fn = std::function<net::awaitable<std::unique_ptr<session>>(net::any_io_executor)>;

        connection_pool(net::any_io_executor ex, pool_params cfg, connect_fn connect);
        ~connection_pool();

        connection_pool(connection_pool const&) = delete;
        connection_pool& operator=(connection_pool const&) = delete;
        connection_pool(connection_pool&&) noexcept;
        connection_pool& operator=(connection_pool&&) noexcept;

        /// 启动池（预建 min_connections 条连接，并启动维护协程）。
        void start();

        /// 借出一条连接。
        /// \note wait_timeout 语义与 client::http_client_pool 一致：<= 0 表示 fail fast
        /// （不等待，池满立即抛超时）；> 0 表示最多等待该时长。
        /// \note 失败统一抛 \ref db_exception：池已停止/未启动时 code 为 operation_canceled，
        ///       等待超时（含 fail fast 未借到）时 code 为 timed_out，可按 code() 分流；
        ///       建连工厂抛出的异常原样透传。
        static constexpr auto default_timeout = std::chrono::seconds(3);
        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout = default_timeout);

        /// 关闭池，唤醒所有等待者。
        void stop();

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;
        net::any_io_executor get_executor() const noexcept;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

      private:
        std::shared_ptr<impl> impl_;
    };

    /**
     * \brief 便捷工厂：按后端名 + 连接串创建连接池。
     * \details 池内每条连接都由 \ref session::connect 建立；backend 相关配置只需一份连接串。
     * 例：`make_pool(ex, p, "mysql", "host=127.0.0.1 user=root password=123456 db=main")`。
     * 各后端支持的连接串键见 \ref mysql_config 与 \ref sqlite_config。
     */
    HTTPLIB_API connection_pool make_pool(net::any_io_executor ex,
                                          std::string_view backend_name,
                                          std::string_view conn_string,
                                          pool_params cfg = {});

} // namespace httplib::db
