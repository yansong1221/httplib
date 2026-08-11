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

    class HTTPLIB_API connection_pool
    {
      private:
        struct impl;

      public:
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

            void release();
        };

      public:
        connection_pool(net::any_io_executor ex, pool_params cfg);
        ~connection_pool();

        connection_pool(connection_pool const&) = delete;
        connection_pool& operator=(connection_pool const&) = delete;
        connection_pool(connection_pool&&) noexcept;
        connection_pool& operator=(connection_pool&&) noexcept;

        void start();
        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout
                                                     = std::chrono::steady_clock::duration::zero());
        void stop();

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;
        net::any_io_executor get_executor() noexcept;

      private:
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
