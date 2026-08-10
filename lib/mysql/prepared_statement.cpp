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

    prepared_statement::prepared_statement(session& sess, std::string sql) : impl_(std::make_unique<impl>())
    {
        impl_->session = &sess;
        impl_->sql = std::move(sql);
    }

    prepared_statement::prepared_statement(prepared_statement&&) noexcept = default;
    prepared_statement& prepared_statement::operator=(prepared_statement&&) noexcept = default;
    prepared_statement::~prepared_statement() = default;

    prepared_statement&
    prepared_statement::bind(std::string_view v)
    {
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string const& v)
    {
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(char const* v)
    {
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(int64_t v)
    {
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(uint64_t v)
    {
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(int v)
    {
        impl_->params.emplace_back(static_cast<int64_t>(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(unsigned v)
    {
        impl_->params.emplace_back(static_cast<uint64_t>(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(short v)
    {
        impl_->params.emplace_back(static_cast<int64_t>(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(unsigned short v)
    {
        impl_->params.emplace_back(static_cast<uint64_t>(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(double v)
    {
        impl_->params.emplace_back(v);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(float v)
    {
        impl_->params.emplace_back(static_cast<double>(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(bool v)
    {
        impl_->params.emplace_back(static_cast<int64_t>(v ? 1 : 0));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::nullptr_t)
    {
        impl_->params.emplace_back();
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(date v)
    {
        impl_->params.emplace_back(boost::mysql::date(v.year, v.month, v.day));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(datetime v)
    {
        impl_->params.emplace_back(
            boost::mysql::datetime(v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(time v)
    {
        impl_->params.emplace_back(std::chrono::hours(v.hour) + std::chrono::minutes(v.minute)
                                   + std::chrono::seconds(v.second) + std::chrono::microseconds(v.microsecond));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(net::const_buffer v)
    {
        impl_->params.emplace_back(boost::mysql::blob_view(static_cast<unsigned char const*>(v.data()), v.size()));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(boost::json::value const& v)
    {
        impl_->data_str = boost::json::serialize(v);
        impl_->params.emplace_back(impl_->data_str);
        return *this;
    }

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
                    if (imp.name_to_idx.find(name) == imp.name_to_idx.end())
                    {
                        size_t idx = imp.param_names.size();
                        imp.param_names.push_back(name);
                        imp.name_to_idx[name] = idx;
                    }
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
    bind_named(prepared_statement::impl& imp, std::string_view name, boost::mysql::field_view fv)
    {
        auto it = imp.name_to_idx.find(std::string(name));
        if (it == imp.name_to_idx.end())
        {
            size_t idx = imp.param_names.size();
            imp.param_names.emplace_back(name);
            imp.name_to_idx[std::string(name)] = idx;
            it = imp.name_to_idx.find(std::string(name));
        }
        auto idx = it->second;
        if (idx >= imp.params.size())
        {
            imp.params.resize(idx + 1);
        }
        imp.params[idx] = fv;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, std::string_view v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, char const* v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(v));
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
        bind_named(*impl_,
                   name,
                   boost::mysql::field_view(std::chrono::hours(v.hour) + std::chrono::minutes(v.minute)
                                            + std::chrono::seconds(v.second)
                                            + std::chrono::microseconds(v.microsecond)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, net::const_buffer v)
    {
        impl_->data_str = std::string(static_cast<char const*>(v.data()), v.size());
        bind_named(
            *impl_,
            name,
            boost::mysql::field_view(boost::mysql::blob_view(static_cast<unsigned char const*>(v.data()), v.size())));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, boost::json::value const& v)
    {
        impl_->data_str = boost::json::serialize(v);
        bind_named(*impl_, name, boost::mysql::field_view(impl_->data_str));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::chrono::system_clock::time_point tp)
    {
        impl_->params.emplace_back(to_datetime(tp));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, std::chrono::system_clock::time_point tp)
    {
        bind_named(*impl_, name, boost::mysql::field_view(to_datetime(tp)));
        return *this;
    }

    net::awaitable<result>
    prepared_statement::execute()
    {
        if (!impl_->parsed && impl_->sql.find(':') != std::string::npos)
        {
            parse_named_params(*impl_);
            impl_->parsed = true;
        }

        auto params = std::exchange(impl_->params, {});
        auto& imp = get_impl(*impl_->session);
        auto start = std::chrono::steady_clock::now();

        boost::mysql::diagnostics diag;
        boost::mysql::results data;
        imp.get_conn().set_meta_mode(boost::mysql::metadata_mode::full);

        try
        {
            if (!impl_->stmt_prepared)
            {
                boost::system::error_code ec;
                impl_->stmt = co_await imp.get_conn().async_prepare_statement(
                    impl_->sql,
                    diag,
                    boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                raise_mysql_error(ec, diag, impl_->sql);
                impl_->stmt_prepared = true;
            }

            boost::system::error_code ec;
            if (params.empty())
            {
                co_await imp.get_conn().async_execute(impl_->stmt.bind(),
                                                      data,
                                                      diag,
                                                      boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            }
            else
            {
                co_await imp.get_conn().async_execute(impl_->stmt.bind(params.begin(), params.end()),
                                                      data,
                                                      diag,
                                                      boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            }
            raise_mysql_error(ec, diag, impl_->sql);
        }
        catch (...)
        {
            impl_->stmt_prepared = false;
            throw;
        }

        auto res = result(std::make_unique<result::impl>(std::move(data)));

        if (imp.query_logger)
        {
            query_log_entry entry;
            entry.sql = impl_->sql;
            entry.duration
                = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            entry.row_count = res.row_count();
            entry.affected_rows = res.affected_rows();
            entry.is_parameterized = !params.empty();
            imp.query_logger(entry);
        }

        if (res.row_count() > 0)
        {
            for (auto& ex : impl_->extractors)
            {
                ex(res);
            }
        }
        co_return res;
    }

    template <typename T, typename F>
    static auto
    into_extractor(std::optional<T>& v, size_t col, F get)
    {
        return [&v, col, get = std::move(get)](result const& r) { v = get(r[0], col); };
    }

    template <typename T, typename F>
    static auto
    into_extractor(std::optional<T>& v, std::string_view name, F get)
    {
        return [&v, n = std::string(name), get = std::move(get)](result const& r) { v = get(r[0], n); };
    }

    prepared_statement&
    prepared_statement::into(std::optional<int64_t>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_int64(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<uint64_t>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_uint64(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<double>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_double(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<float>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_float(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<bool>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_bool(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<std::string>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_string(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<date>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_date(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<datetime>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_datetime(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<time>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_time(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<std::chrono::system_clock::time_point>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_timestamp(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<net::const_buffer>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_blob(c); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<boost::json::value>& v, size_t col)
    {
        impl_->extractors.push_back(into_extractor(v, col, [](auto r, auto c) { return r.as_json(c); }));
        return *this;
    }

    prepared_statement&
    prepared_statement::into(std::optional<date>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_date(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<int64_t>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_int64(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<uint64_t>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_uint64(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<double>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_double(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<float>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_float(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<bool>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_bool(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<std::string>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_string(n); }));
        return *this;
    }

    prepared_statement&
    prepared_statement::into(std::optional<datetime>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_datetime(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<time>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_time(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<std::chrono::system_clock::time_point>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_timestamp(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<net::const_buffer>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_blob(n); }));
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::optional<boost::json::value>& v, std::string_view name)
    {
        impl_->extractors.push_back(into_extractor(v, name, [](auto r, auto n) { return r.as_json(n); }));
        return *this;
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
