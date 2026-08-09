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

    inline constexpr char const* conn_key = "httplib.db.conn";

    struct db_middleware_options
    {
        bool auto_transaction = false;
    };

    class HTTPLIB_API db_middleware
    {
      public:
        explicit db_middleware(std::shared_ptr<mysql::connection_pool> pool, db_middleware_options opts = {});
        ~db_middleware();
        db_middleware(db_middleware const&);
        db_middleware& operator=(db_middleware const&);
        db_middleware(db_middleware&&) noexcept;
        db_middleware& operator=(db_middleware&&) noexcept;

        net::awaitable<bool> before(request& req, response& resp);
        net::awaitable<bool> after(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

    inline mysql::session&
    get_session(request& req)
    {
        if (!req.has_custom_data(conn_key))
        {
            throw std::runtime_error("No DB session in request. "
                                     "Did you register db_middleware?");
        }
        return *req.custom_data<mysql::session*>(conn_key);
    }

    inline std::shared_ptr<mysql::connection_pool>
    get_connection_pool(request& req)
    {
        return req.custom_data<std::shared_ptr<mysql::connection_pool>>("httplib.db.pool_ref");
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
