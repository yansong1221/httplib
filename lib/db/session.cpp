#include "httplib/db/session.hpp"
#include "httplib/db/exception.hpp"
#include "prepared_statement_impl.h"
#include "registry.hpp"
#include "render.hpp"
#include <boost/system/error_code.hpp>
#include <utility>

namespace httplib::db
{

    struct session::impl
    {
        std::unique_ptr<detail::backend> backend;
        bool in_transaction = false;
        bool live = true;
        session::query_logger query_logger;

        std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_ping;

        void
        touch()
        {
            last_active = std::chrono::steady_clock::now();
        }
    };

    session::session(std::unique_ptr<impl> p) : impl_(std::move(p)) {}
    session::session(session&&) noexcept = default;
    session& session::operator=(session&&) noexcept = default;
    session::~session() = default;

    net::awaitable<session>
    session::connect(net::any_io_executor ex, mysql_config cfg)
    {
        co_return co_await connect_internal(ex, "mysql", cfg.to_options());
    }

    net::awaitable<session>
    session::connect(net::any_io_executor ex, sqlite_config cfg)
    {
        co_return co_await connect_internal(ex, "sqlite", cfg.to_options());
    }

    net::awaitable<session>
    session::connect(net::any_io_executor ex, std::string_view backend_name, std::string_view conn_string)
    {
        co_return co_await connect_internal(ex, backend_name, options::parse(conn_string));
    }

    net::awaitable<session>
    session::connect_internal(net::any_io_executor ex, std::string_view backend_name, options opts)
    {
        detail::register_backends();
        auto const* factory = detail::find_backend(backend_name);
        if (!factory)
        {
            throw db_exception(boost::system::error_code {},
                               "db: unknown backend '" + std::string(backend_name)
                                   + "' (registered: " + detail::registered_backend_names() + ")");
        }
        auto imp = std::make_unique<impl>();
        imp->backend = (*factory)(ex, opts);
        co_await imp->backend->connect();
        imp->live = true;
        co_return session(std::move(imp));
    }

    net::awaitable<result>
    session::query(std::string_view sql)
    {
        auto& imp = *impl_;
        auto start = std::chrono::steady_clock::now();

        result res;
        try
        {
            res = co_await imp.backend->execute(sql);
        }
        catch (db_exception const&)
        {
            if (!imp.backend->alive())
            {
                imp.live = false;
            }
            throw;
        }
        imp.touch();

        if (imp.query_logger)
        {
            query_log_entry entry;
            entry.sql = std::string(sql);
            entry.duration = std::chrono::steady_clock::now() - start;
            entry.row_count = res.row_count();
            entry.affected_rows = res.affected_rows();
            imp.query_logger(entry);
        }
        co_return res;
    }

    net::awaitable<result>
    session::execute_query(std::string_view sql, std::vector<detail::binder> binders)
    {
        auto& imp = *impl_;

        // 总是渲染：有绑定则替换占位符；无绑定（纯文本）也校验 SQL 中残留的 `:name`，
        // 与 prepared_statement 语义一致（未绑定命名参数 → 抛）。
        auto rendered = detail::render_query(sql, binders, *imp.backend);

        // 含命名绑定时解析占位符名字（供异常信息与日志使用）。
        std::vector<std::string> names;
        bool all_positional = true;
        for (auto const& b : binders)
        {
            if (!b.name.empty())
            {
                all_positional = false;
                break;
            }
        }
        if (!all_positional)
        {
            auto [_, parsed_names] = detail::parse_placeholders(sql);
            names = std::move(parsed_names);
        }

        co_return co_await execute_rendered(rendered.sql,
                                            std::move(rendered.params),
                                            sql,
                                            names,
                                            !rendered.expanded);
    }

    net::awaitable<result>
    session::execute_rendered(std::string_view sql,
                              std::vector<detail::param> params,
                              std::string_view original_sql,
                              std::vector<std::string> const& names,
                              bool cacheable)
    {
        auto& imp = *impl_;
        auto start = std::chrono::steady_clock::now();

        result res;
        try
        {
            res = co_await imp.backend->execute(sql, params, cacheable);
        }
        catch (db_exception const& ex)
        {
            if (!imp.backend->alive())
            {
                imp.live = false;
            }
            throw detail::enrich_error(ex, original_sql, names, params);
        }
        imp.touch();

        if (imp.query_logger)
        {
            query_log_entry entry;
            entry.sql = std::string(original_sql);
            entry.duration = std::chrono::steady_clock::now() - start;
            entry.row_count = res.row_count();
            entry.affected_rows = res.affected_rows();
            entry.is_parameterized = !params.empty();
            imp.query_logger(entry);
        }
        co_return res;
    }

    prepared_statement
    session::stmt(std::string_view sql)
    {
        return prepared_statement(*this, std::string(sql));
    }

    net::awaitable<void>
    session::begin_transaction()
    {
        auto& imp = *impl_;
        co_await imp.backend->begin();
        imp.in_transaction = true;
    }

    net::awaitable<void>
    session::commit()
    {
        auto& imp = *impl_;
        co_await imp.backend->commit();
        imp.in_transaction = false;
    }

    net::awaitable<void>
    session::rollback()
    {
        auto& imp = *impl_;
        co_await imp.backend->rollback();
        imp.in_transaction = false;
    }

    net::awaitable<bool>
    session::ping()
    {
        auto& imp = *impl_;
        bool ok = co_await imp.backend->ping();
        imp.live = ok;
        imp.last_ping = std::chrono::steady_clock::now();
        co_return ok;
    }

    net::awaitable<void>
    session::reconnect()
    {
        auto& imp = *impl_;
        co_await imp.backend->reconnect();
        imp.live = true;
        imp.in_transaction = false;
    }

    void
    session::set_query_logger(query_logger cb)
    {
        impl_->query_logger = std::move(cb);
    }

    void
    session::touch()
    {
        impl_->touch();
    }

    bool
    session::is_live() const
    {
        return impl_->live;
    }

    std::chrono::steady_clock::time_point
    session::last_active_time() const
    {
        return impl_->last_active;
    }

    std::chrono::steady_clock::time_point
    session::last_ping_time() const
    {
        return impl_->last_ping;
    }

    bool
    session::in_transaction() const
    {
        return impl_->in_transaction;
    }

} // namespace httplib::db
