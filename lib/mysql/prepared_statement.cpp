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
    prepared_statement::bind(std::chrono::steady_clock::duration v)
    {
        impl_->params.emplace_back(std::chrono::duration_cast<std::chrono::microseconds>(v));
        return *this;
    }

    static boost::mysql::datetime
    to_datetime(std::chrono::system_clock::time_point tp)
    {
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
        auto days = epoch / 86400;
        auto secs = epoch % 86400;
        if (secs < 0)
        {
            secs += 86400;
            --days;
        }

        days += 719468;
        auto era = (days >= 0 ? days : days - 146096) / 146097;
        auto doe = days - era * 146097;
        auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        auto y = yoe + era * 400;
        auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        auto mp = (5 * doy + 2) / 153;
        auto d = doy - (153 * mp + 2) / 5 + 1;
        auto m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2)
        {
            ++y;
        }

        return boost::mysql::datetime(static_cast<unsigned short>(y),
                                      static_cast<unsigned short>(m),
                                      static_cast<unsigned short>(d),
                                      static_cast<unsigned short>(secs / 3600),
                                      static_cast<unsigned short>((secs % 3600) / 60),
                                      static_cast<unsigned short>(secs % 60),
                                      0);
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
    prepared_statement::bind(std::string_view name, std::chrono::steady_clock::duration v)
    {
        bind_named(*impl_, name, boost::mysql::field_view(std::chrono::duration_cast<std::chrono::microseconds>(v)));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind_timestamp(std::chrono::system_clock::time_point tp)
    {
        impl_->params.emplace_back(to_datetime(tp));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind_timestamp(std::string_view name, std::chrono::system_clock::time_point tp)
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

        auto result_impl = std::make_unique<result::impl>();
        imp.pooled.get().set_meta_mode(boost::mysql::metadata_mode::full);

        try
        {
            if (params.empty())
            {
                co_await imp.pooled.get().async_execute(impl_->stmt.bind(),
                                                        result_impl->data,
                                                        boost::asio::use_awaitable);
            }
            else
            {
                co_await imp.pooled.get().async_execute(impl_->stmt.bind(params.begin(), params.end()),
                                                        result_impl->data,
                                                        boost::asio::use_awaitable);
            }
        }
        catch (...)
        {
            impl_->stmt_prepared = false;
            throw;
        }

        build_result_impl(*result_impl);
        auto res = result(std::move(result_impl));
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

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
