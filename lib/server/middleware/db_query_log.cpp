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
        if (!has<db_middleware>(req))
        {
            return true;
        }

        auto opts = impl_->opts;
        auto log = std::make_shared<query_log_options::value_type>();
        auto sess = fetch<db_middleware>(req);

        sess->get()->set_query_logger(
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
        if (!has<db_query_log_middleware>(req))
        {
            return true;
        }

        auto log = fetch<db_query_log_middleware>(req);

        if (impl_->opts.on_request_complete)
        {
            impl_->opts.on_request_complete(req, *log);
        }

        if (has<db_middleware>(req))
        {
            auto& sess = fetch<db_middleware>(req);
            sess->get()->set_query_logger({});
        }
        return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
