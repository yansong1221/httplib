#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/prepared_statement.hpp"
#include "mysql/result_impl.h"
#include "mysql/session_impl.h"
#include "httplib/mysql/session.hpp"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>

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
        impl_->params.emplace_back(
            std::chrono::hours(v.hour) + std::chrono::minutes(v.minute) + std::chrono::seconds(v.second)
            + std::chrono::microseconds(v.microsecond));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(net::const_buffer v)
    {
        impl_->params.emplace_back(
            boost::mysql::blob_view(static_cast<unsigned char const*>(v.data()), v.size()));
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
        bind_named(*impl_,
                   name,
                   boost::mysql::field_view(
                       boost::mysql::blob_view(static_cast<unsigned char const*>(v.data()), v.size())));
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

        if (!impl_->stmt_prepared)
        {
            impl_->stmt = co_await imp.pooled.get().async_prepare_statement(impl_->sql, boost::asio::use_awaitable);
            impl_->stmt_prepared = true;
        }

        boost::mysql::results data;
        imp.pooled.get().set_meta_mode(boost::mysql::metadata_mode::full);

        try
        {
            if (params.empty())
            {
                co_await imp.pooled.get().async_execute(impl_->stmt.bind(),
                                                        data,
                                                        boost::asio::use_awaitable);
            }
            else
            {
                co_await imp.pooled.get().async_execute(impl_->stmt.bind(params.begin(), params.end()),
                                                        data,
                                                        boost::asio::use_awaitable);
            }
        }
        catch (...)
        {
            impl_->stmt_prepared = false;
            throw;
        }

        auto res = result(std::make_unique<result::impl>(std::move(data)));
        for (auto& ex : impl_->extractors)
        {
            ex(res);
        }
        co_return res;
    }

    prepared_statement&
    prepared_statement::into(int64_t& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_int64(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(uint64_t& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_uint64(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(double& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_double(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(float& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_float(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(bool& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_bool(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::string& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_string(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(date& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_date(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(datetime& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_datetime(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(time& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_time(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::chrono::system_clock::time_point& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_timestamp(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(net::const_buffer& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_blob(col); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(boost::json::value& v, size_t col)
    {
        impl_->extractors.push_back([&v, col](result const& r) { v = r[0].as_json(col); });
        return *this;
    }

    prepared_statement&
    prepared_statement::into(date& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r) { v = r[0].as_date(n); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(int64_t& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r)
            {
                auto row = r[0];
                v = row.get<int64_t>(n);
            });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(uint64_t& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r)
            {
                auto row = r[0];
                v = row.get<uint64_t>(n);
            });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(double& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r)
            {
                auto row = r[0];
                v = row.get<double>(n);
            });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(float& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r)
            {
                auto row = r[0];
                v = row.get<float>(n);
            });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(bool& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r)
            {
                auto row = r[0];
                v = row.get<bool>(n);
            });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::string& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r)
            {
                auto row = r[0];
                v = row.get<std::string>(n);
            });
        return *this;
    }

    prepared_statement&
    prepared_statement::into(datetime& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r) { v = r[0].as_datetime(n); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(time& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r) { v = r[0].as_time(n); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(std::chrono::system_clock::time_point& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r) { v = r[0].as_timestamp(n); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(net::const_buffer& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r) { v = r[0].as_blob(n); });
        return *this;
    }
    prepared_statement&
    prepared_statement::into(boost::json::value& v, std::string_view name)
    {
        impl_->extractors.push_back(
            [&v, n = std::string(name)](result const& r) { v = r[0].as_json(n); });
        return *this;
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
