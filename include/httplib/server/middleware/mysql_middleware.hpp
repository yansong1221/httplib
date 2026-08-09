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

    inline constexpr char const* mysql_conn_key = "httplib.mysql.conn";

    struct mysql_middleware_options
    {
        bool auto_transaction = false;
    };

    class HTTPLIB_API mysql_middleware
    {
      public:
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

    inline mysql::session&
    get_mysql_session(request& req)
    {
        if (!req.has_custom_data(mysql_conn_key))
        {
            throw std::runtime_error("No MySQL session in request. "
                                     "Did you register mysql_middleware?");
        }
        return *req.custom_data<mysql::session*>(mysql_conn_key);
    }

    inline std::shared_ptr<mysql::connection_pool>
    get_mysql_pool(request& req)
    {
        return req.custom_data<std::shared_ptr<mysql::connection_pool>>("httplib.mysql.pool_ref");
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
