#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/config.hpp"
#include "httplib/mysql/connection_pool.hpp"
#include "httplib/mysql/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>
#include <stdexcept>

namespace httplib::server::middleware
{

    struct mysql_middleware_options
    {
        bool auto_transaction = false;
        std::chrono::steady_clock::duration acquire_timeout = std::chrono::seconds(5);
    };

    class HTTPLIB_API mysql_middleware
    {
      public:
        using value_type = std::shared_ptr<mysql::connection_pool::session_handle>;
        using pool_type = std::shared_ptr<mysql::connection_pool>;

        inline static std::shared_ptr<mysql::connection_pool>
        fetch_pool(request& req)
        {
            return req.data().fetch<std::shared_ptr<mysql::connection_pool>>();
        }

        explicit mysql_middleware(std::shared_ptr<mysql::connection_pool> pool, mysql_middleware_options opts = {});
        ~mysql_middleware();
        mysql_middleware(mysql_middleware const&);
        mysql_middleware& operator=(mysql_middleware const&);
        mysql_middleware(mysql_middleware&&) noexcept;
        mysql_middleware& operator=(mysql_middleware&&) noexcept;

        net::awaitable<bool> before(request& req, response& resp);
        net::awaitable<bool> after(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
