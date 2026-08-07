#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/server/middleware/db_query_log.hpp"
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <any>
#include <utility>

namespace httplib::server::middleware
{

    namespace
    {

        class logging_db_connection : public db::db_connection
        {
          public:
            logging_db_connection(std::shared_ptr<db::db_connection> real,
                                  std::shared_ptr<std::vector<query_log_entry>> log,
                                  std::chrono::microseconds slow_threshold,
                                  query_log_options::slow_query_callback slow_cb)
                : real_(std::move(real)), log_(std::move(log)), slow_threshold_(slow_threshold),
                  slow_cb_(std::move(slow_cb))
            {
            }

            net::awaitable<db::db_result>
            query(std::string_view sql) override
            {
                auto start = std::chrono::steady_clock::now();
                auto result = co_await real_->query(sql);
                record_entry(std::string(sql), start, result, false);
                co_return result;
            }

            net::awaitable<db::db_result>
            query(std::string_view sql, std::span<const std::string> params) override
            {
                auto start = std::chrono::steady_clock::now();
                auto result = co_await real_->query(sql, params);
                record_entry(std::string(sql), start, result, true);
                co_return result;
            }

            net::awaitable<db::db_result>
            execute(std::string_view sql) override
            {
                auto start = std::chrono::steady_clock::now();
                auto result = co_await real_->execute(sql);
                record_entry(std::string(sql), start, result, false);
                co_return result;
            }

            net::awaitable<db::db_result>
            execute(std::string_view sql, std::span<const std::string> params) override
            {
                auto start = std::chrono::steady_clock::now();
                auto result = co_await real_->execute(sql, params);
                record_entry(std::string(sql), start, result, true);
                co_return result;
            }

            net::awaitable<void>
            begin_transaction() override
            {
                return real_->begin_transaction();
            }
            net::awaitable<void>
            commit() override
            {
                return real_->commit();
            }
            net::awaitable<void>
            rollback() override
            {
                return real_->rollback();
            }
            bool
            in_transaction() const override
            {
                return real_->in_transaction();
            }
            bool
            is_alive() const override
            {
                return real_->is_alive();
            }
            net::awaitable<bool>
            ping() override
            {
                return real_->ping();
            }
            std::string_view
            backend() const override
            {
                return real_->backend();
            }
            std::chrono::steady_clock::time_point
            last_active_time() const override
            {
                return real_->last_active_time();
            }
            std::chrono::steady_clock::time_point
            last_ping_time() const override
            {
                return real_->last_ping_time();
            }
            void
            touch() override
            {
                real_->touch();
            }

          private:
            void
            record_entry(std::string sql,
                         std::chrono::steady_clock::time_point start,
                         const db::db_result& result,
                         bool parameterized)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);

                query_log_entry entry {.sql = std::move(sql),
                                       .duration = elapsed,
                                       .row_count = result.rows.size(),
                                       .affected_rows = result.affected_rows,
                                       .is_parameterized = parameterized};

                if (slow_threshold_.count() > 0 && elapsed >= slow_threshold_ && slow_cb_)
                {
                    slow_cb_(entry);
                }

                log_->push_back(std::move(entry));
            }

            std::shared_ptr<db::db_connection> real_;
            std::shared_ptr<std::vector<query_log_entry>> log_;
            std::chrono::microseconds slow_threshold_;
            query_log_options::slow_query_callback slow_cb_;
        };

    } // namespace

    struct query_log_context
    {
        std::shared_ptr<std::vector<query_log_entry>> log_entries;
        std::shared_ptr<db::db_connection> real_conn;
    };

    static constexpr const char* k_query_log_context_key = "httplib.db.query_log.ctx";

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
        auto real_conn = get_db_connection(req);

        auto log_entries = std::make_shared<std::vector<query_log_entry>>();
        auto logging_conn = std::make_shared<logging_db_connection>(
            real_conn, log_entries, impl_->opts.slow_query_threshold, impl_->opts.on_slow_query);

        req.set_custom_data(db::db_connection_pool::conn_key,
                            std::static_pointer_cast<db::db_connection>(logging_conn));

        auto ctx = std::make_shared<query_log_context>();
        ctx->log_entries = log_entries;
        ctx->real_conn = std::move(real_conn);
        req.set_custom_data(k_query_log_context_key, std::any(ctx));

        return true;
    }

    bool
    db_query_log_middleware::after(request& req, response&)
    {
        if (!req.has_custom_data(k_query_log_context_key))
        {
            return true;
        }

        auto ctx = req.custom_data<std::shared_ptr<query_log_context>>(k_query_log_context_key);

        req.set_custom_data(db::db_connection_pool::conn_key, std::any(ctx->real_conn));

        if (impl_->opts.on_request_complete)
        {
            impl_->opts.on_request_complete(req, *ctx->log_entries);
        }

        req.erase_custom_data(k_query_log_context_key);
        return true;
    }

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
