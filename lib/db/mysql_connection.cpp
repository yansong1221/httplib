#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/mysql_connection.hpp"
#include "db/stmt_cache.h"
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/statement.hpp>
#include <charconv>
#include <stdexcept>

namespace httplib::db
{

    mysql_connection::mysql_connection(net::any_io_executor ex, size_t stmt_cache_size)
        : ex_(std::move(ex))
        , conn_(ex_)
        , stmt_cache_(std::make_unique<stmt_cache>(stmt_cache_size))
        , last_active_(std::chrono::steady_clock::now())
    {
    }

    mysql_connection::~mysql_connection()
    {
        stmt_cache_->clear();
    }

    void
    mysql_connection::validate_charset(const std::string& charset)
    {
        for (char c : charset)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            {
                throw std::invalid_argument("db_config: invalid charset '" + charset + "'");
            }
        }
    }

    net::awaitable<std::shared_ptr<mysql_connection>>
    mysql_connection::create(net::any_io_executor ex, const db_config& config)
    {
        auto conn = std::shared_ptr<mysql_connection>(new mysql_connection(ex, config.stmt_cache_size));

        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(config.host, config.port);
        params.username = config.user;
        params.password = config.password;
        params.database = config.database;

        co_await conn->conn_.async_connect(params, boost::asio::use_awaitable);
        conn->alive_ = true;

        conn->conn_.set_meta_mode(boost::mysql::metadata_mode::full);

        if (!config.charset.empty())
        {
            validate_charset(config.charset);
            boost::mysql::results charset_result;
            std::string set_names_sql = "SET NAMES '" + config.charset + "'";
            co_await conn->conn_.async_execute(set_names_sql, charset_result, boost::asio::use_awaitable);
        }

        conn->touch();
        co_return conn;
    }

    net::awaitable<std::shared_ptr<mysql_connection>>
    mysql_connection::create(boost::asio::io_context& io_ctx, const db_config& config)
    {
        co_return co_await create(io_ctx.get_executor(), config);
    }

    db_connection_factory
    mysql_connection::make_factory()
    {
        return [](net::any_io_executor ex,
                  const db_config& config) -> net::awaitable<std::shared_ptr<db_connection>>
        {
            co_return co_await mysql_connection::create(ex, config);
        };
    }

#if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable : 4996)
#endif

    net::awaitable<db_result>
    mysql_connection::query(std::string_view sql)
    {
        boost::mysql::results boost_results;
        co_await conn_.async_execute(std::string(sql), boost_results, boost::asio::use_awaitable);
        touch();
        co_return convert_results(boost_results);
    }

    net::awaitable<db_result>
    mysql_connection::query(std::string_view sql, std::span<const std::string> params)
    {
        if (params.empty())
        {
            co_return co_await query(sql);
        }

        auto stmt = co_await get_or_prepare(sql);

        std::vector<boost::mysql::field_view> fields;
        fields.reserve(params.size());
        for (const auto& p : params)
        {
            fields.emplace_back(p);
        }

        boost::mysql::results boost_results;
        bool need_retry = false;
        try
        {
            co_await conn_.async_execute(stmt.bind(fields.begin(), fields.end()),
                                         boost_results,
                                         boost::asio::use_awaitable);
        }
        catch (...)
        {
            stmt_cache_->erase(sql);
            need_retry = true;
        }

        if (need_retry)
        {
            auto fresh_stmt
                = co_await conn_.async_prepare_statement(std::string(sql), boost::asio::use_awaitable);

            co_await conn_.async_execute(fresh_stmt.bind(fields.begin(), fields.end()),
                                         boost_results,
                                         boost::asio::use_awaitable);

            auto evicted = stmt_cache_->insert(std::string(sql), std::move(fresh_stmt));
            if (evicted)
            {
                try
                {
                    co_await conn_.async_close_statement(*evicted, boost::asio::use_awaitable);
                }
                catch (...)
                {
                }
            }
        }

        touch();
        co_return convert_results(boost_results);
    }

    net::awaitable<boost::mysql::statement>
    mysql_connection::get_or_prepare(std::string_view sql)
    {
        auto* cached = stmt_cache_->find(sql);
        if (cached)
        {
            co_return *cached;
        }

        std::string sql_str(sql);
        auto stmt = co_await conn_.async_prepare_statement(sql_str, boost::asio::use_awaitable);

        auto evicted = stmt_cache_->insert(sql_str, stmt);
        if (evicted)
        {
            try
            {
                co_await conn_.async_close_statement(*evicted, boost::asio::use_awaitable);
            }
            catch (...)
            {
            }
        }

        co_return stmt;
    }

    net::awaitable<db_result>
    mysql_connection::execute(std::string_view sql)
    {
        return query(sql);
    }

    net::awaitable<db_result>
    mysql_connection::execute(std::string_view sql, std::span<const std::string> params)
    {
        return query(sql, params);
    }

#if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
    #pragma warning(pop)
#endif

    net::awaitable<void>
    mysql_connection::begin_transaction()
    {
        boost::mysql::results r;
        co_await conn_.async_execute("START TRANSACTION", r, boost::asio::use_awaitable);
        in_transaction_ = true;
        touch();
    }

    net::awaitable<void>
    mysql_connection::commit()
    {
        boost::mysql::results r;
        co_await conn_.async_execute("COMMIT", r, boost::asio::use_awaitable);
        in_transaction_ = false;
        touch();
    }

    net::awaitable<void>
    mysql_connection::rollback()
    {
        boost::mysql::results r;
        co_await conn_.async_execute("ROLLBACK", r, boost::asio::use_awaitable);
        in_transaction_ = false;
        touch();
    }

    bool
    mysql_connection::in_transaction() const
    {
        return in_transaction_;
    }

    bool
    mysql_connection::is_alive() const
    {
        return alive_;
    }

    net::awaitable<bool>
    mysql_connection::ping()
    {
        try
        {
            co_await conn_.async_ping(boost::asio::use_awaitable);
            alive_ = true;
            last_ping_ = std::chrono::steady_clock::now();
            touch();
            co_return true;
        }
        catch (...)
        {
            alive_ = false;
            co_return false;
        }
    }

    std::string_view
    mysql_connection::backend() const
    {
        return "mysql";
    }

    std::chrono::steady_clock::time_point
    mysql_connection::last_active_time() const
    {
        return last_active_;
    }

    std::chrono::steady_clock::time_point
    mysql_connection::last_ping_time() const
    {
        return last_ping_;
    }

    void
    mysql_connection::touch()
    {
        last_active_ = std::chrono::steady_clock::now();
    }

    db_result
    mysql_connection::convert_results(const boost::mysql::results& boost_results)
    {
        db_result result;

        if (!boost_results.has_value())
        {
            return result;
        }

        result.affected_rows = boost_results.affected_rows();
        result.insert_id = boost_results.last_insert_id();

        auto meta = boost_results.meta();
        if (meta.empty())
        {
            return result;
        }

        result.columns.reserve(meta.size());
        for (const auto& col : meta)
        {
            auto sv = col.column_name();
            result.columns.emplace_back(sv.data(), sv.size());
        }

        auto rows_view = boost_results.rows();
        result.rows.reserve(rows_view.size());
        size_t col_count = result.columns.size();

        for (auto row : rows_view)
        {
            std::vector<std::string> db_row;
            db_row.reserve(col_count);
            for (size_t i = 0; i < col_count && i < row.size(); ++i)
            {
                const auto& field = row.at(i);

                if (field.is_null())
                {
                    db_row.emplace_back();
                }
                else if (field.is_int64())
                {
                    char buf[24];
                    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), field.as_int64());
                    db_row.emplace_back(buf, ptr);
                }
                else if (field.is_uint64())
                {
                    char buf[24];
                    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), field.as_uint64());
                    db_row.emplace_back(buf, ptr);
                }
                else if (field.is_double())
                {
                    char buf[32];
                    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), field.as_double());
                    db_row.emplace_back(buf, ptr);
                }
                else if (field.is_string())
                {
                    db_row.emplace_back(field.as_string());
                }
                else if (field.is_blob())
                {
                    auto blob = field.as_blob();
                    db_row.emplace_back(reinterpret_cast<const char*>(blob.data()), blob.size());
                }
                else if (field.is_date())
                {
                    auto d = field.as_date();
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", d.year(), d.month(), d.day());
                    db_row.emplace_back(buf);
                }
                else if (field.is_datetime())
                {
                    auto dt = field.as_datetime();
                    char buf[32];
                    std::snprintf(buf,
                                  sizeof(buf),
                                  "%04u-%02u-%02u %02u:%02u:%02u",
                                  dt.year(),
                                  dt.month(),
                                  dt.day(),
                                  dt.hour(),
                                  dt.minute(),
                                  dt.second());
                    db_row.emplace_back(buf);
                }
                else if (field.is_time())
                {
                    auto t = field.as_time();
                    auto total_secs = std::chrono::duration_cast<std::chrono::seconds>(t).count();
                    bool negative = total_secs < 0;
                    if (negative)
                    {
                        total_secs = -total_secs;
                    }
                    auto hours = static_cast<long long>(total_secs / 3600);
                    auto minutes = static_cast<long long>((total_secs % 3600) / 60);
                    auto secs = static_cast<long long>(total_secs % 60);
                    char buf[24];
                    if (negative)
                    {
                        std::snprintf(buf, sizeof(buf), "-%02lld:%02lld:%02lld", hours, minutes, secs);
                    }
                    else
                    {
                        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", hours, minutes, secs);
                    }
                    db_row.emplace_back(buf);
                }
                else
                {
                    db_row.emplace_back();
                }
            }
            result.rows.push_back(std::move(db_row));
        }

        return result;
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
