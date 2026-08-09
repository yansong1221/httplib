#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/config.hpp"
#include "httplib/mysql/mysql_fwd.hpp"
#include <memory>

namespace httplib::mysql
{

    class HTTPLIB_API connection_pool
    {
      public:
        connection_pool(net::any_io_executor ex, config cfg);
        ~connection_pool();

        connection_pool(connection_pool const&) = delete;
        connection_pool& operator=(connection_pool const&) = delete;
        connection_pool(connection_pool&&) noexcept;
        connection_pool& operator=(connection_pool&&) noexcept;

        void start();
        net::awaitable<session> async_acquire(std::chrono::steady_clock::duration wait_timeout
                                                 = std::chrono::steady_clock::duration::zero());
        void stop();

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
