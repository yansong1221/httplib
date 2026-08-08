#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/db_query_log.hpp"
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    static constexpr const char* k_query_log_key = "httplib.db.query_log";

    class db_query_log_middleware::impl
    {
      public:
        query_log_options opts;

        explicit impl(query_log_options o)
            : opts(std::move(o))
        {
        }
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
    db_query_log_middleware&
    db_query_log_middleware::operator=(db_query_log_middleware&&) noexcept = default;

    bool
    db_query_log_middleware::before(request& req, response&)
    {
        if (!req.has_custom_data(db::db_pool::conn_key))
        {
            return true;
        }

        auto log = std::make_shared<std::vector<db::query_log_entry>>();
        auto opts = impl_->opts;

        auto& sess = get_db_session(req);
        sess.set_query_logger(
            [log, opts](const db::query_log_entry& entry) mutable
            {
                if (opts.slow_query_threshold.count() > 0 && entry.duration >= opts.slow_query_threshold
                    && opts.on_slow_query)
                {
                    opts.on_slow_query(entry);
                }
                log->push_back(entry);
            });

        req.set_custom_data(k_query_log_key, std::any(log));
        return true;
    }

    bool
    db_query_log_middleware::after(request& req, response&)
    {
        if (!req.has_custom_data(k_query_log_key))
        {
            return true;
        }

        auto log
            = req.custom_data<std::shared_ptr<std::vector<db::query_log_entry>>>(k_query_log_key);

        if (impl_->opts.on_request_complete)
        {
            impl_->opts.on_request_complete(req, *log);
        }

        if (req.has_custom_data(db::db_pool::conn_key))
        {
            auto& sess = get_db_session(req);
            sess.set_query_logger({});
        }

        req.erase_custom_data(k_query_log_key);
        return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
