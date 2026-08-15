#pragma once

#include "config.hpp"
#include "fwd.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
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
        std::chrono::seconds idle_timeout { 300 };
        std::chrono::seconds idle_check_interval { 60 };
        std::chrono::seconds health_check_interval { 30 };
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
        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout
                                                     = std::chrono::steady_clock::duration::zero());

        /// 关闭池，唤醒所有等待者。
        void stop();

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;
        net::any_io_executor get_executor() noexcept;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

      private:
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::db
