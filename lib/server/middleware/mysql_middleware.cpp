#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/mysql_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    static constexpr char const* k_tx_key = "httplib.db.tx";

    class mysql_middleware::impl
    {
      public:
        std::shared_ptr<mysql::connection_pool> pool;
        mysql_middleware_options opts;

        impl(std::shared_ptr<mysql::connection_pool> p, mysql_middleware_options o) : pool(std::move(p)), opts(std::move(o)) {}
    };

    mysql_middleware::mysql_middleware(std::shared_ptr<mysql::connection_pool> pool, mysql_middleware_options opts)
        : impl_(std::make_unique<impl>(std::move(pool), std::move(opts)))
    {
    }

    mysql_middleware::~mysql_middleware() = default;

    mysql_middleware::mysql_middleware(mysql_middleware const& other)
        : impl_(std::make_unique<impl>(other.impl_->pool, other.impl_->opts))
    {
    }

    mysql_middleware&
    mysql_middleware::operator=(mysql_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(other.impl_->pool, other.impl_->opts);
        }
        return *this;
    }

    mysql_middleware::mysql_middleware(mysql_middleware&&) noexcept = default;
    mysql_middleware& mysql_middleware::operator=(mysql_middleware&&) noexcept = default;

    net::awaitable<bool>
    mysql_middleware::before(request& req, response&)
    {
        auto session = co_await impl_->pool->async_acquire();
        auto* ptr = new mysql::session(std::move(session));
        req.set_custom_data(mysql_conn_key, std::any(ptr));
        req.set_custom_data("httplib.mysql.pool_ref", std::any(impl_->pool));

        if (impl_->opts.auto_transaction)
        {
            auto tx = co_await get_mysql_session(req).begin();
            req.set_custom_data(k_tx_key, std::any(new mysql::transaction(std::move(tx))));
        }

        co_return true;
    }

    net::awaitable<bool>
    mysql_middleware::after(request& req, response&)
    {
        if (impl_->opts.auto_transaction && req.has_custom_data(k_tx_key))
        {
            auto* tx = req.custom_data<mysql::transaction*>(k_tx_key);
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

        if (req.has_custom_data(mysql_conn_key))
        {
            delete req.custom_data<mysql::session*>(mysql_conn_key);
            req.erase_custom_data(mysql_conn_key);
            req.erase_custom_data("httplib.mysql.pool_ref");
        }

        co_return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
