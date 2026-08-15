#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/config.hpp"
#include "httplib/mysql/mysql_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <memory>

namespace httplib::mysql
{

    /**
     * \brief MySQL 连接池。
     * \details
     * 维护一批可复用的连接，通过 \ref async_acquire 借出、句柄析构时归还。
     * \n
     * 后台协程负责空闲连接的回收与健康检查（见 \ref pool_params）。
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

            /**
             * \brief 提前归还连接。
             */
            void release();
        };

      public:
        connection_pool(net::any_io_executor ex, pool_params cfg);
        ~connection_pool();

        connection_pool(connection_pool const&) = delete;
        connection_pool& operator=(connection_pool const&) = delete;
        connection_pool(connection_pool&&) noexcept;
        connection_pool& operator=(connection_pool&&) noexcept;

        /**
         * \brief 启动池（预建 min_connections 条连接，并启动维护协程）。
         */
        void start();

        /**
         * \brief 借出一条连接。
         * \param wait_timeout 池满时的等待超时；0（默认）表示不设超时，一直等到有连接可用。
         * \returns 连接句柄。
         * \throws std::runtime_error 池已关闭或等待超时。
         */
        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout
                                                     = std::chrono::steady_clock::duration::zero());

        /**
         * \brief 关闭池，唤醒所有等待者。
         */
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

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
