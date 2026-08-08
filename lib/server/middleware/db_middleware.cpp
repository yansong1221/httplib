#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    static constexpr char const* k_tx_key = "httplib.db.tx";

    class db_middleware::impl
    {
      public:
        std::shared_ptr<db::db_pool> pool;
        db_middleware_options opts;

        impl(std::shared_ptr<db::db_pool> p, db_middleware_options o) : pool(std::move(p)), opts(std::move(o)) {}
    };

    db_middleware::db_middleware(std::shared_ptr<db::db_pool> pool, db_middleware_options opts)
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
    db_middleware& db_middleware::operator=(db_middleware&&) noexcept = default;

    net::awaitable<bool>
    db_middleware::before(request& req, response&)
    {
        auto session = co_await impl_->pool->get_session();
        auto* ptr = new db::db_session(std::move(session));
        req.set_custom_data(db::db_pool::conn_key, std::any(ptr));
        req.set_custom_data("httplib.db.pool_ref", std::any(impl_->pool));

        if (impl_->opts.auto_transaction)
        {
            auto tx = co_await get_db_session(req).begin();
            req.set_custom_data(k_tx_key, std::any(new db::transaction(std::move(tx))));
        }

        co_return true;
    }

    net::awaitable<bool>
    db_middleware::after(request& req, response&)
    {
        if (impl_->opts.auto_transaction && req.has_custom_data(k_tx_key))
        {
            auto* tx = req.custom_data<db::transaction*>(k_tx_key);
            if (tx)
            {
                try
                {
                    co_await tx->commit();
                }
                catch (...)
                {
                }
                delete tx;
            }
            req.erase_custom_data(k_tx_key);
        }

        if (req.has_custom_data(db::db_pool::conn_key))
        {
            delete req.custom_data<db::db_session*>(db::db_pool::conn_key);
            req.erase_custom_data(db::db_pool::conn_key);
            req.erase_custom_data("httplib.db.pool_ref");
        }

        co_return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
