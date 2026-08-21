#include "httplib/db/session.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/util/string_hash.hpp"
#include "registry.hpp"
#include "render.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/system/error_code.hpp>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace httplib::db
{

    struct session::impl : public std::enable_shared_from_this<impl>
    {
        net::any_io_executor ex_;
        std::unique_ptr<detail::backend> backend;
        bool in_transaction = false;
        bool live = true;
        session::query_logger query_logger;

        /// prepared statement 统一缓存（LRU；capacity 0 表示不缓存）。
        struct cached_statement
        {
            detail::statement_handle handle;
            std::list<std::string>::iterator lru_it;
        };
        util::string_map<cached_statement> stmt_cache;
        std::list<std::string> stmt_lru;
        size_t stmt_cache_capacity = 64;

        std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_ping;

        void
        touch()
        {
            last_active = std::chrono::steady_clock::now();
        }

        /// 后端失效时标记会话死亡；返回后端是否已失效。
        bool
        mark_dead_if_needed()
        {
            if (backend->alive())
            {
                return false;
            }
            live = false;
            return true;
        }

        /// 释放所有缓存语句并清空缓存（连接失效 / 重连 / 析构路径复用）。
        net::awaitable<void>
        clear_stmt_cache()
        {
            for (auto& [key, cs] : stmt_cache)
            {
                co_await backend->close_statement(cs.handle);
            }
            stmt_cache.clear();
            stmt_lru.clear();
            co_return;
        }

        /// 析构/归还路径：把自身交给后台协程，异步 close 所有缓存语句后自毁。
        void
        close_statements_async()
        {
            if (stmt_cache.empty())
            {
                return;
            }
            net::co_spawn(
                ex_,
                [self = shared_from_this()]() -> net::awaitable<void> { co_await self->clear_stmt_cache(); },
                [](std::exception_ptr) {});
        }

        /// 取缓存语句；未命中则 prepare 并按 LRU 逐出后插入。
        net::awaitable<detail::statement_handle>
        acquire_cached(std::string_view sql)
        {
            if (auto it = stmt_cache.find(sql); it != stmt_cache.end())
            {
                stmt_lru.splice(stmt_lru.begin(), stmt_lru, it->second.lru_it);
                co_return it->second.handle;
            }

            auto h = co_await backend->prepare(sql);
            while (stmt_cache.size() >= stmt_cache_capacity)
            {
                auto evict_key = std::move(stmt_lru.back());
                stmt_lru.pop_back();
                auto evict_it = stmt_cache.find(evict_key);
                if (evict_it == stmt_cache.end())
                {
                    break;
                }
                co_await backend->close_statement(evict_it->second.handle);
                stmt_cache.erase(evict_it);
            }
            stmt_lru.emplace_front(sql);
            stmt_cache.emplace(sql, cached_statement { h, stmt_lru.begin() });
            co_return h;
        }

        /// 记录一次查询日志（query_logger 为空时跳过）。
        void
        log_query(std::string_view sql,
                  std::chrono::steady_clock::duration duration,
                  size_t row_count,
                  uint64_t affected_rows,
                  bool is_parameterized) const
        {
            if (!query_logger)
            {
                return;
            }
            query_log_entry entry;
            entry.sql = std::string(sql);
            entry.duration = duration;
            entry.row_count = row_count;
            entry.affected_rows = affected_rows;
            entry.is_parameterized = is_parameterized;
            query_logger(entry);
        }
    };

    session::session(std::shared_ptr<impl> p) : impl_(std::move(p)) {}
    session::session(session&&) noexcept = default;

    session&
    session::operator=(session&& other) noexcept
    {
        if (this != &other)
        {
            detach();
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    session::~session() { detach(); }

    void
    session::detach() noexcept
    {
        if (!impl_)
        {
            return;
        }

        // 会话析构无法 co_await：把 impl 交给后台协程，异步 close 所有缓存语句后自毁。
        // shared_ptr 保证 backend / stmt_cache 在协程运行期间存活。
        impl_->close_statements_async();
        impl_.reset();
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
        auto imp = std::make_shared<impl>();
        imp->ex_ = ex;
        imp->backend = (*factory)(ex, opts);
        int const cache_cap = opts.as_int("max_cached_statements").value_or(static_cast<int>(imp->stmt_cache_capacity));
        // 负数视为禁用语句缓存（-1 曾因 cast 成 SIZE_MAX 导致缓存无上限）。
        imp->stmt_cache_capacity = cache_cap < 0 ? 0 : static_cast<size_t>(cache_cap);
        co_await imp->backend->connect();
        imp->live = true;
        co_return session(std::move(imp));
    }

    net::awaitable<result>
    session::query(std::string_view sql)
    {
        auto start = std::chrono::steady_clock::now();

        result res;
        try
        {
            res = co_await impl_->backend->execute(sql);
        }
        catch (db_exception const&)
        {
            impl_->mark_dead_if_needed();
            throw;
        }
        impl_->touch();
        impl_->log_query(sql, std::chrono::steady_clock::now() - start, res.row_count(), res.affected_rows(), false);
        co_return res;
    }

    net::awaitable<result>
    session::execute_query(std::string_view sql, std::vector<detail::binder> binders, bool cacheable)
    {
        // 总是渲染：有绑定则替换占位符；无绑定（纯文本）也校验 SQL 中残留的 `:name`，
        // 与 prepared_statement 语义一致（未绑定命名参数 → 抛）。
        auto rendered = detail::render_query(sql, binders, *impl_->backend);

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

        co_return co_await execute_rendered(rendered.sql, std::move(rendered.params), sql, names, cacheable);
    }

    net::awaitable<result>
    session::execute_rendered(std::string_view sql,
                              std::vector<detail::param> params,
                              std::string_view original_sql,
                              std::vector<std::string> const& names,
                              bool cacheable)
    {
        auto start = std::chrono::steady_clock::now();

        // 参数化语句是单语句：多语句（语句级分号）语义不明确，且后端 prepare 只执行第一条，
        // 会静默截断。统一在入口拒绝，避免后端行为不一致（此前仅 SQLite 后端检测）。
        if (!params.empty() && detail::split_statements(sql).size() > 1)
        {
            throw db_exception(boost::system::error_code {},
                               "db: parameterized multi-statement SQL is not supported; "
                               "split the statements");
        }

        result res;
        std::optional<db_exception> failure;
        try
        {
            if (params.empty())
            {
                res = co_await impl_->backend->execute(sql);
            }
            else if (cacheable && impl_->stmt_cache_capacity > 0)
            {
                auto handle = co_await impl_->acquire_cached(sql);
                res = co_await impl_->backend->execute_statement(handle, params);
            }
            else
            {
                // 不缓存（capacity 0 或占位符数量随参数变化）：每次 prepare → 执行 → close。
                auto h = co_await impl_->backend->prepare(sql);
                std::exception_ptr e;
                try
                {
                    res = co_await impl_->backend->execute_statement(h, params);
                }
                catch (...)
                {
                    e = std::current_exception();
                }
                co_await impl_->backend->close_statement(h);
                if (e)
                {
                    std::rethrow_exception(e);
                }
            }
        }
        catch (db_exception const& ex)
        {
            // MSVC 不允许在 catch 块内 co_await，先保存异常，清理与 enrich 在块外做。
            failure = ex;
        }
        if (failure)
        {
            if (impl_->mark_dead_if_needed())
            {
                // 连接失效后服务器端语句全部作废，清空缓存避免复用坏句柄。
                co_await impl_->clear_stmt_cache();
            }
            throw detail::enrich_error(*failure, original_sql, names, params);
        }
        impl_->touch();
        impl_->log_query(original_sql,
                         std::chrono::steady_clock::now() - start,
                         res.row_count(),
                         res.affected_rows(),
                         !params.empty());
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
        try
        {
            co_await impl_->backend->begin();
        }
        catch (db_exception const&)
        {
            impl_->mark_dead_if_needed();
            throw;
        }
        impl_->in_transaction = true;
    }

    net::awaitable<void>
    session::commit()
    {
        try
        {
            co_await impl_->backend->commit();
        }
        catch (db_exception const&)
        {
            impl_->mark_dead_if_needed();
            throw;
        }
        impl_->in_transaction = false;
    }

    net::awaitable<void>
    session::rollback()
    {
        try
        {
            co_await impl_->backend->rollback();
        }
        catch (db_exception const&)
        {
            impl_->mark_dead_if_needed();
            throw;
        }
        impl_->in_transaction = false;
    }

    net::awaitable<bool>
    session::ping()
    {
        bool ok = co_await impl_->backend->ping();
        impl_->live = ok;
        impl_->last_ping = std::chrono::steady_clock::now();
        co_return ok;
    }

    net::awaitable<void>
    session::reconnect()
    {
        // 断开重连后服务器端 prepared statement 全部失效，先释放所有缓存句柄。
        co_await impl_->clear_stmt_cache();
        co_await impl_->backend->reconnect();
        impl_->live = true;
        impl_->in_transaction = false;
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

    net::any_io_executor
    session::get_executor() const
    {
        return impl_->ex_;
    }

} // namespace httplib::db
