#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/db_query_log.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    class db_query_log_middleware::impl
    {
      public:
        query_log_options opts;

        explicit impl(query_log_options o) : opts(std::move(o)) {}
    };

    db_query_log_middleware::db_query_log_middleware(query_log_options opts)
        : impl_(std::make_unique<impl>(std::move(opts)))
    {
    }

    db_query_log_middleware::~db_query_log_middleware() = default;

    db_query_log_middleware::db_query_log_middleware(db_query_log_middleware const& other)
        : impl_(std::make_unique<impl>(other.impl_->opts))
    {
    }

    db_query_log_middleware&
    db_query_log_middleware::operator=(db_query_log_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(other.impl_->opts);
        }
        return *this;
    }

    db_query_log_middleware::db_query_log_middleware(db_query_log_middleware&&) noexcept = default;
    db_query_log_middleware& db_query_log_middleware::operator=(db_query_log_middleware&&) noexcept = default;

    bool
    db_query_log_middleware::before(request& req, response&)
    {
        // 单次加锁完成判断+取值：has() 后再取值是两次加锁，中间若有并发 erase() 会漏判。
        auto sess = fetch<db_middleware>(req);
        if (!sess)
        {
            return true;
        }

        auto opts = impl_->opts;
        auto log = std::make_shared<query_log_options::value_type>();

// sess 是 optional<shared_ptr<session_handle>>：先脱掉 optional 拿到 shared_ptr，
        // 再 get() 才是 session_handle::get()，后一个 get() 拿到 db::session。
        sess.value()->get()->set_query_logger(
            [log, opts](db::query_log_entry const& entry) mutable
            {
                if (opts.slow_query_threshold.count() > 0 && entry.duration >= opts.slow_query_threshold
                    && opts.on_slow_query)
                {
                    opts.on_slow_query(entry);
                }
                log->push_back(entry);
            });

        store<db_query_log_middleware>(req, std::move(log));
        return true;
    }

    bool
    db_query_log_middleware::after(request& req, response&)
    {
        auto log = fetch<db_query_log_middleware>(req);
        if (!log)
        {
            return true;
        }

        if (impl_->opts.on_request_complete)
        {
            impl_->opts.on_request_complete(req, **log);
        }

        auto sess = fetch<db_middleware>(req);
        if (sess)
        {
            sess.value()->get()->set_query_logger({});
        }
        return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
