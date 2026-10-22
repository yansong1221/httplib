#pragma once
#include "httplib/client/client.hpp"
#include "httplib/client/client_fwd.hpp"
#include "httplib/url/scheme.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>

namespace httplib::client
{

    /**
     * \brief HTTP 客户端连接池参数。
     * \details \c max_size 为每个 host（route）的连接数上限，\c max_total 为全局连接数上限。
     * 两者对 `0` 的语义不同：`max_size == 0` 表示不再新建连接（已有 idle 连接仍可复用），
     * `max_total == 0` 表示全局不限。其余字段在新建连接时透传给 \c http_client。
     */
    struct pool_params
    {
        /// 每个 host（route）的连接数上限；`0` 表示不再新建连接，
        /// 已存在的 idle 连接仍可复用；没有可复用连接时 acquire 会等待或超时。
        size_t max_size = 100;
        /// 全局连接数上限；`0` 表示不限。
        size_t max_total = 0;
        /// 空闲连接过期时间；`0` 表示不按 idle_timeout 回收。
        std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(60);
        /// 空闲回收检查周期；`0` 表示使用默认 `60s`。
        std::chrono::steady_clock::duration idle_check_interval = std::chrono::seconds(60);
        bool validate_on_borrow = false;

        http_client::timeout_policy timeout_policy = http_client::timeout_policy::overall;
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(30);
        int max_redirects = 0;
        bool verify_ssl = true;
        std::string ca_cert;
    };

    class HTTPLIB_API http_client_pool
    {
      private:
        class impl;

      public:
        class HTTPLIB_API client_handle
        {
            friend class http_client_pool;

            std::weak_ptr<impl> pool_;
            std::unique_ptr<http_client> conn_;
            boost::system::error_code error_;

            client_handle(std::weak_ptr<impl> pool, std::unique_ptr<http_client> conn);

          public:
            client_handle();
            explicit client_handle(boost::system::error_code ec);

            client_handle(client_handle const&) = delete;
            client_handle& operator=(client_handle const&) = delete;

            client_handle(client_handle&& other) noexcept;
            client_handle& operator=(client_handle&& other) noexcept;

            ~client_handle();

            http_client* get();
            http_client const* get() const;

            explicit operator bool() const noexcept;
            bool has_error() const noexcept;

            http_client* operator->();
            http_client const* operator->() const;

            http_client& operator*();
            http_client const& operator*() const;

            boost::system::error_code const& error() const noexcept;

            void release();
        };

        struct pool_stats
        {
            size_t idle = 0;
            size_t active = 0;
        };

      public:
        explicit http_client_pool(net::any_io_executor const& ex, pool_params params = {});
        ~http_client_pool();

        http_client_pool(http_client_pool const&) = delete;
        http_client_pool& operator=(http_client_pool const&) = delete;

        http_client_pool(http_client_pool&& other) noexcept;
        http_client_pool& operator=(http_client_pool&& other) noexcept;

        // wait_timeout controls how long async_acquire() waits for a connection when
        // the pool is at capacity. A value of zero (or negative) makes the call fail
        // fast: it returns a handle carrying `error::acquire_timeout` immediately if
        // no connection is available without waiting.
        static constexpr auto default_timeout = std::chrono::seconds(3);

        net::awaitable<client_handle> async_acquire(std::string_view host,
                                                    uint16_t port,
                                                    url::scheme s,
                                                    std::chrono::steady_clock::duration wait_timeout = default_timeout);

        net::awaitable<client_handle> async_acquire(std::string_view url,
                                                    std::chrono::steady_clock::duration wait_timeout = default_timeout);

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        /// \note 计数为原子读，可从任意线程调用。
        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;

        /// \brief 关闭池（终态，不可重启），唤醒所有等待者。
        /// \details 实际清理在池的 strand 上（ticker 的 on_stop）执行。调用方应保证
        /// **先关闭池、再停止或析构其执行器**（池的析构函数会自动调用本函数）：
        /// 执行器停止后，已投递的清理任务与归还协程不会再被派发，连接要等到
        /// `io_context::restart()` 后再次 `run()`，或随执行器析构才回收。
        /// \note 与 Asio 的通用语义一致，本函数**不取消**未完成的操作与回调；需要
        /// 立即断开时，应在停止执行器前先关闭/取消对应连接。
        /// \note 执行器停止期间 \ref active_count() / \ref idle_count() /
        /// \ref total_count() 不再更新，不代表当前可用连接数。
        void stop();

        /// \return 在池执行器上完成读取后变为 ready 的 future；不阻塞调用线程。
        /// \note 入参在调用时拷贝，返回的 future 不引用调用方缓冲区。
        std::future<pool_stats> stats(std::string_view host, uint16_t port, url::scheme s) const;
        std::future<pool_stats> stats(std::string_view url) const;

        /// \brief stats 的异步版本：先切到池执行器再读取，可从任意线程调用。
        net::awaitable<pool_stats> async_stats(std::string_view host, uint16_t port, url::scheme s) const;
        net::awaitable<pool_stats> async_stats(std::string_view url) const;

      private:
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
