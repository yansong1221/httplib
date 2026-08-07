#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    class db_middleware::impl
    {
      public:
        std::shared_ptr<db::db_connection_pool> pool;
        db_middleware_options opts;

        impl(std::shared_ptr<db::db_connection_pool> p, db_middleware_options o)
            : pool(std::move(p)), opts(std::move(o))
        {
        }
    };

    db_middleware::db_middleware(std::shared_ptr<db::db_connection_pool> pool, db_middleware_options opts)
        : impl_(std::make_unique<impl>(std::move(pool), std::move(opts)))
    {
    }

    db_middleware::~db_middleware() = default;

    db_middleware::db_middleware(db_middleware const& other)
        : impl_(std::make_unique<impl>(other.impl_->pool, other.impl_->opts))
    {
    }

    db_middleware&
    db_middleware::operator=(db_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(other.impl_->pool, other.impl_->opts);
        }
        return *this;
    }

    db_middleware::db_middleware(db_middleware&&) noexcept = default;
    db_middleware&
    db_middleware::operator=(db_middleware&&) noexcept = default;

    net::awaitable<bool>
    db_middleware::before(request& req, response&)
    {
        auto conn = co_await impl_->pool->acquire();
        req.set_custom_data(db::db_connection_pool::conn_key, std::any(conn));

        if (impl_->opts.inject_pool)
        {
            req.set_custom_data(db::db_connection_pool::pool_key, std::any(impl_->pool));
        }

        if (impl_->opts.auto_transaction)
        {
            co_await conn->begin_transaction();
        }

        co_return true;
    }

    net::awaitable<bool>
    db_middleware::after(request& req, response&)
    {
        if (!req.has_custom_data(db::db_connection_pool::conn_key))
        {
            co_return true;
        }

        auto conn
            = req.custom_data<std::shared_ptr<db::db_connection>>(db::db_connection_pool::conn_key);

        if (impl_->opts.auto_transaction && conn->in_transaction())
        {
            try
            {
                co_await conn->commit();
            }
            catch (...)
            {
            }
        }

        impl_->pool->release(std::move(conn));
        req.erase_custom_data(db::db_connection_pool::conn_key);
        req.erase_custom_data(db::db_connection_pool::pool_key);
        co_return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
