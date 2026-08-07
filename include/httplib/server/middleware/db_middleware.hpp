#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/config.hpp"
#include "httplib/db/db_connection.hpp"
#include "httplib/db/db_connection_pool.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>
#include <stdexcept>

namespace httplib::server::middleware
{

    struct db_middleware_options
    {
        bool auto_transaction = false;
        bool inject_pool = true;
    };

    class HTTPLIB_API db_middleware
    {
      public:
        explicit db_middleware(std::shared_ptr<db::db_connection_pool> pool, db_middleware_options opts = {});
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

    inline std::shared_ptr<db::db_connection>
    get_db_connection(const request& req)
    {
        if (!req.has_custom_data(db::db_connection_pool::conn_key))
        {
            throw std::runtime_error("No DB connection in request. "
                                     "Did you register db_middleware?");
        }
        return req.custom_data<std::shared_ptr<db::db_connection>>(db::db_connection_pool::conn_key);
    }

    inline std::shared_ptr<db::db_connection_pool>
    get_db_pool(const request& req)
    {
        if (!req.has_custom_data(db::db_connection_pool::pool_key))
        {
            throw std::runtime_error("No DB pool in request. "
                                     "Did you register db_middleware with inject_pool=true?");
        }
        return req.custom_data<std::shared_ptr<db::db_connection_pool>>(db::db_connection_pool::pool_key);
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
