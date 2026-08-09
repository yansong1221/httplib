#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/mysql_query_log.hpp"
#include "httplib/server/middleware/mysql_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>

namespace httplib::server::middleware
{

    static constexpr char const* k_query_log_key = "httplib.db.query_log";

    class mysql_query_log_middleware::impl
    {
      public:
        query_log_options opts;

        explicit impl(query_log_options o) : opts(std::move(o)) {}
    };

    mysql_query_log_middleware::mysql_query_log_middleware(query_log_options opts)
        : impl_(std::make_unique<impl>(std::move(opts)))
    {
    }

    mysql_query_log_middleware::~mysql_query_log_middleware() = default;

    mysql_query_log_middleware::mysql_query_log_middleware(mysql_query_log_middleware const& other)
        : impl_(std::make_unique<impl>(other.impl_->opts))
    {
    }

    mysql_query_log_middleware&
    mysql_query_log_middleware::operator=(mysql_query_log_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(other.impl_->opts);
        }
        return *this;
    }

    mysql_query_log_middleware::mysql_query_log_middleware(mysql_query_log_middleware&&) noexcept = default;
    mysql_query_log_middleware& mysql_query_log_middleware::operator=(mysql_query_log_middleware&&) noexcept = default;

    bool
    mysql_query_log_middleware::before(request& req, response&)
    {
        if (!req.has_custom_data(mysql_conn_key))
        {
            return true;
        }

        auto log = std::make_shared<std::vector<mysql::query_log_entry>>();
        auto opts = impl_->opts;

        auto& sess = get_mysql_session(req);
        sess.set_query_logger(
            [log, opts](mysql::query_log_entry const& entry) mutable
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
    mysql_query_log_middleware::after(request& req, response&)
    {
        if (!req.has_custom_data(k_query_log_key))
        {
            return true;
        }

        auto log = req.custom_data<std::shared_ptr<std::vector<mysql::query_log_entry>>>(k_query_log_key);

        if (impl_->opts.on_request_complete)
        {
            impl_->opts.on_request_complete(req, *log);
        }

        if (req.has_custom_data(mysql_conn_key))
        {
            auto& sess = get_mysql_session(req);
            sess.set_query_logger({});
        }

        req.erase_custom_data(k_query_log_key);
        return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
