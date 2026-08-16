#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/config.hpp"
#include "httplib/db/connection_pool.hpp"
#include "httplib/db/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>
#include <stdexcept>

namespace httplib::server::middleware
{

    struct db_middleware_options
    {
        bool auto_transaction = false;
        std::chrono::steady_clock::duration acquire_timeout = std::chrono::seconds(5);
    };

    class HTTPLIB_API db_middleware
    {
      public:
        using value_type = std::shared_ptr<db::connection_pool::session_handle>;
        using pool_type = std::shared_ptr<db::connection_pool>;

        inline static std::shared_ptr<db::connection_pool>
        fetch_pool(request& req)
        {
            return req.data().fetch<std::shared_ptr<db::connection_pool>>();
        }

        explicit db_middleware(std::shared_ptr<db::connection_pool> pool, db_middleware_options opts = {});
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

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
