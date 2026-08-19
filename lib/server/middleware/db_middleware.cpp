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
        std::shared_ptr<db::connection_pool> pool;
        db_middleware_options opts;

        impl(std::shared_ptr<db::connection_pool> p, db_middleware_options o) : pool(std::move(p)), opts(std::move(o))
        {
        }
    };

    db_middleware::db_middleware(std::shared_ptr<db::connection_pool> pool, db_middleware_options opts)
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
        auto handle = std::make_shared<db::connection_pool::session_handle>(
            co_await impl_->pool->async_acquire(impl_->opts.acquire_timeout));

        req.data().store(handle);
        req.data().store(impl_->pool);

        if (impl_->opts.auto_transaction)
        {
            co_await handle->get()->begin_transaction();
        }

        co_return true;
    }

    net::awaitable<bool>
    db_middleware::after(request& req, response& resp)
    {
        if (impl_->opts.auto_transaction && req.data().has<value_type>())
        {
            auto sess = req.data().fetch<value_type>();
            if (sess)
            {
                // 仅请求成功（<400）时提交；失败状态回滚，避免把业务错误落库。
                bool const ok = resp.result_int() < 400;
                std::exception_ptr op_err;
                try
                {
                    if (ok)
                    {
                        co_await sess->get()->commit();
                    }
                    else
                    {
                        co_await sess->get()->rollback();
                    }
                }
                catch (...)
                {
                    op_err = std::current_exception();
                }
                if (op_err)
                {
                    // 提交失败时尝试回滚；回滚失败则放弃（连接交给 pool 处理）。
                    try
                    {
                        co_await sess->get()->rollback();
                    }
                    catch (...)
                    {
                    }
                    if (ok)
                    {
                        resp.set_error_content(http::status::internal_server_error);
                    }
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
