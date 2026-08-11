#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/mysql_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    class mysql_middleware::impl
    {
      public:
        std::shared_ptr<mysql::connection_pool> pool;
        mysql_middleware_options opts;

        impl(std::shared_ptr<mysql::connection_pool> p, mysql_middleware_options o)
            : pool(std::move(p))
            , opts(std::move(o))
        {
        }
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
        auto handle
            = std::make_shared<mysql::connection_pool::session_handle>(co_await impl_->pool->async_acquire(impl_->opts.acquire_timeout));

        req.data().store(handle);
        req.data().store(impl_->pool);

        if (impl_->opts.auto_transaction)
        {
            co_await handle->get()->begin_transaction();
        }

        co_return true;
    }

    net::awaitable<bool>
    mysql_middleware::after(request& req, response&)
    {
        if (impl_->opts.auto_transaction && req.data().has<value_type>())
        {
            auto sess = req.data().fetch<value_type>();
            if (sess)
            {
                try
                {
                    co_await sess->get()->commit();
                }
                catch (...)
                {
                }
            }
        }

        if (req.data().has<value_type>())
        {
            req.data().erase<value_type>();
            req.data().erase<pool_type>();
        }

        co_return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
