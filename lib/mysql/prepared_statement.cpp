#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/prepared_statement.hpp"
#include "httplib/mysql/mysql_exception.hpp"
#include "httplib/mysql/session.hpp"
#include "mysql/result_impl.h"
#include "mysql/session_impl.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <format>

namespace httplib::mysql
{

    static boost::mysql::datetime
    to_datetime(std::chrono::system_clock::time_point tp)
    {
        auto days = std::chrono::floor<std::chrono::days>(tp);
        auto ymd = std::chrono::year_month_day(days);
        auto hms = std::chrono::hh_mm_ss(tp - days);

        return boost::mysql::datetime(static_cast<unsigned short>(static_cast<int>(ymd.year())),
                                      static_cast<unsigned short>(static_cast<unsigned>(ymd.month())),
                                      static_cast<unsigned short>(static_cast<unsigned>(ymd.day())),
                                      static_cast<unsigned short>(hms.hours().count()),
                                      static_cast<unsigned short>(hms.minutes().count()),
                                      static_cast<unsigned short>(hms.seconds().count()),
                                      static_cast<unsigned long>(hms.subseconds().count()));
    }

    static void
    parse_named_params(prepared_statement::impl& imp)
    {
        std::string result;
        result.reserve(imp.sql.size());

        imp.param_names.clear();

        for (size_t i = 0; i < imp.sql.size(); ++i)
        {
            char c = imp.sql[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                char quote = c;
                result += c;
                while (++i < imp.sql.size())
                {
                    result += imp.sql[i];
                    if (imp.sql[i] == quote && (i + 1 >= imp.sql.size() || imp.sql[i + 1] != quote))
                    {
                        break;
                    }
                    if (imp.sql[i] == '\\' && i + 1 < imp.sql.size())
                    {
                        result += imp.sql[++i];
                    }
                }
                continue;
            }
            if (c == '-' && i + 1 < imp.sql.size() && imp.sql[i + 1] == '-')
            {
                while (i < imp.sql.size() && imp.sql[i] != '\n')
                {
                    ++i;
                }
                if (i < imp.sql.size())
                {
                    result += imp.sql[i];
                }
                continue;
            }
            if (c == '/' && i + 1 < imp.sql.size() && imp.sql[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < imp.sql.size() && !(imp.sql[i] == '*' && imp.sql[i + 1] == '/'))
                {
                    ++i;
                }
                i += 1;
                continue;
            }
            if (c == ':')
            {
                size_t start = i + 1;
                while (start < imp.sql.size()
                       && (std::isalnum(static_cast<unsigned char>(imp.sql[start])) || imp.sql[start] == '_'))
                {
                    ++start;
                }
                if (start > i + 1)
                {
                    std::string name = imp.sql.substr(i + 1, start - i - 1);
                    imp.param_names.push_back(name);
                    result += '?';
                    i = start - 1;
                    continue;
                }
            }
            result += c;
        }
        imp.sql = std::move(result);
    }

    static void
    bind_named(prepared_statement::impl& imp, std::string_view name, prepared_statement::impl::param value)
    {
        auto key = std::string(name);
        if (std::ranges::find(imp.param_names, name) == imp.param_names.end())
        {
            throw std::runtime_error("db: no such named parameter '" + key + "'");
        }
        if (imp.has_positional_bind)
        {
            throw std::runtime_error("db: cannot mix positional and named parameters");
        }
        imp.has_named_bind = true;
        imp.named_values[std::move(key)] = std::move(value);
    }

    prepared_statement::prepared_statement(session& sess, std::string sql) : impl_(std::make_unique<impl>())
    {
        impl_->session = &sess;
        impl_->original_sql = sql;
        impl_->sql = std::move(sql);
        if (impl_->sql.find(':') != std::string::npos)
        {
            parse_named_params(*impl_);
        }
    }

    prepared_statement::prepared_statement(prepared_statement&&) noexcept = default;
    prepared_statement& prepared_statement::operator=(prepared_statement&&) noexcept = default;
    prepared_statement::~prepared_statement() = default;

    prepared_statement&
    prepared_statement::bind(std::string_view v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(std::string(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string const& v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(char const* v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(std::string(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(int64_t v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(uint64_t v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(int v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(static_cast<int64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(unsigned v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(static_cast<uint64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(short v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(static_cast<int64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(unsigned short v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(static_cast<uint64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(double v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(float v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(static_cast<double>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(bool v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(static_cast<int64_t>(v ? 1 : 0)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::nullptr_t)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view());
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(date v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(boost::mysql::date(v.year, v.month, v.day)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(datetime v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(
            boost::mysql::datetime(v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(time v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(v.to_duration()));
        return *this;
    }

    // blob 以零拷贝视图存储（非拥有，与字符串不同）：调用方需保证缓冲区在 execute() 前保持有效
    prepared_statement&
    prepared_statement::bind(std::span<std::byte const> v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::mysql::field_view(
            boost::mysql::blob_view(reinterpret_cast<unsigned char const*>(v.data()), v.size())));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(boost::json::value const& v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::json::serialize(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, std::string_view v)
    {
        bind_named(*impl_, name, std::string(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, char const* v)
    {
        bind_named(*impl_, name, std::string(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, int64_t v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, uint64_t v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, int v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(static_cast<int64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, unsigned v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(static_cast<uint64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, short v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(static_cast<int64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, unsigned short v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(static_cast<uint64_t>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, double v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, float v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(static_cast<double>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, bool v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(static_cast<int64_t>(v ? 1 : 0)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, std::nullptr_t)
    {
        bind_named(*impl_, name, boost::mysql::field_view());
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, date v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(boost::mysql::date(v.year, v.month, v.day)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, datetime v)
    {
        bind_named(*impl_,
                   name,
                   boost::mysql::field_view(
                       boost::mysql::datetime(v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, time v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(v.to_duration()));
        return *this;
    }

    // blob 以零拷贝视图存储（非拥有，与字符串不同）：调用方需保证缓冲区在 execute() 前保持有效
    prepared_statement&
    prepared_statement::bind(std::string_view name, std::span<std::byte const> v)
    {
        bind_named(*impl_,
                   name,
                   boost::mysql::field_view(
                       boost::mysql::blob_view(reinterpret_cast<unsigned char const*>(v.data()), v.size())));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, boost::json::value const& v)
    {
        bind_named(*impl_, name, boost::json::serialize(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::chrono::system_clock::time_point tp)
    {
        impl_->begin_bind();
        auto& imp = get_impl(*impl_->session);
        impl_->params.emplace_back(boost::mysql::field_view(to_datetime(tp + imp.utc_offset)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, std::chrono::system_clock::time_point tp)
    {
        auto& imp = get_impl(*impl_->session);
        bind_named(*impl_, name, boost::mysql::field_view(to_datetime(tp + imp.utc_offset)));
        return *this;
    }

    net::awaitable<result>
    prepared_statement::execute()
    {
        // 命名绑定（或未绑定的 :name 语句）→ 从 named_values 重建参数；纯位置绑定 → 直接用 params
        if (impl_->has_named_bind || (!impl_->has_positional_bind && !impl_->param_names.empty()))
        {
            impl_->params.clear();
            impl_->params.reserve(impl_->param_names.size());
            for (auto const& name : impl_->param_names)
            {
                auto it = impl_->named_values.find(name);
                if (it == impl_->named_values.end())
                {
                    throw std::runtime_error("db: unbound named parameter '" + name + "'");
                }
                impl_->params.push_back(it->second);
            }
        }

        // 统一投影成 field_view：标量/视图直接拷贝，JSON（std::string 分支）引用其持有缓冲区
        std::vector<boost::mysql::field_view> views;
        views.reserve(impl_->params.size());
        for (auto const& p : impl_->params)
        {
            std::visit(
                [&](auto const& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::string>)
                    {
                        views.emplace_back(v);
                    }
                    else
                    {
                        views.push_back(v);
                    }
                },
                p);
        }

        auto make_params_str = [&]() -> std::string
        { return impl_->has_named_bind ? format_named_params(impl_->param_names, views) : format_params(views); };
        auto& imp = get_impl(*impl_->session);
        auto start = std::chrono::steady_clock::now();

        boost::mysql::diagnostics diag;
        boost::mysql::results data;

        boost::mysql::statement stmt;
        if (auto* cached = imp.find_statement(impl_->sql))
        {
            stmt = *cached;
        }
        else
        {
            boost::system::error_code ec;
            stmt = co_await imp.get_conn().async_prepare_statement(
                impl_->sql,
                diag,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec)
            {
                imp.raise_error(ec, diag, impl_->original_sql, make_params_str());
            }
            imp.store_statement(impl_->sql, stmt);
        }

        boost::system::error_code ec;
        if (views.empty())
        {
            co_await imp.get_conn().async_execute(stmt.bind(),
                                                  data,
                                                  diag,
                                                  boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        }
        else
        {
            co_await imp.get_conn().async_execute(stmt.bind(views.begin(), views.end()),
                                                  data,
                                                  diag,
                                                  boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        }
        if (ec)
        {
            imp.raise_error(ec, diag, impl_->original_sql, make_params_str());
        }

        auto res = result(std::make_unique<result::impl>(std::move(data), imp.utc_offset));

        if (imp.query_logger)
        {
            query_log_entry entry;
            entry.sql = impl_->original_sql;
            entry.duration = std::chrono::steady_clock::now() - start;
            entry.row_count = res.row_count();
            entry.affected_rows = res.affected_rows();
            entry.is_parameterized = !views.empty();
            imp.query_logger(entry);
        }

        if (res.row_count() > 0)
        {
            for (auto& ex : impl_->extractors)
            {
                ex(res);
            }
        }
        impl_->need_params_reset = true;
        impl_->need_extractors_reset = true;
        co_return res;
    }

    void
    prepared_statement::add_extractor(std::function<void(result const&)> ex)
    {
        impl_->add_extractor(std::move(ex));
    }

    size_t
    prepared_statement::alloc_into_col()
    {
        return impl_->alloc_into_col();
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
