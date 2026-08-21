#ifdef HTTPLIB_ENABLED_DATABASE
#include "mysql_backend.hpp"
#include "httplib/db/exception.hpp"
#include "registry.hpp"
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <charconv>

namespace httplib::db::detail
{
    namespace
    {
        // 把公开的标量参数转成 Boost.MySQL 字段视图。
        boost::mysql::field_view
        to_field_view(param const& p, std::chrono::seconds utc_offset)
        {
            return std::visit(
                [&](auto const& v) -> boost::mysql::field_view
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                    {
                        return {};
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        return boost::mysql::field_view(v);
                    }
                    else if constexpr (std::is_same_v<T, uint64_t>)
                    {
                        return boost::mysql::field_view(v);
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        return boost::mysql::field_view(v);
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        return boost::mysql::field_view(v);
                    }
                    else if constexpr (std::is_same_v<T, std::span<std::byte const>>)
                    {
                        return boost::mysql::field_view(
                            boost::mysql::blob_view(reinterpret_cast<unsigned char const*>(v.data()), v.size()));
                    }
                    else if constexpr (std::is_same_v<T, date>)
                    {
                        return boost::mysql::field_view(boost::mysql::date(v.year, v.month, v.day));
                    }
                    else if constexpr (std::is_same_v<T, datetime>)
                    {
                        return boost::mysql::field_view(
                            boost::mysql::datetime(v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond));
                    }
                    else if constexpr (std::is_same_v<T, time>)
                    {
                        return boost::mysql::field_view(v.to_duration());
                    }
                    else
                    {
                        // time_point（UTC）：TIMESTAMP 列语义，存会话时区墙上时钟（+offset），
                        // 服务器会按会话时区换算回 UTC；读回时由 to_field 还原成 UTC time_point。
                        auto dt = datetime::from_time_point(v + utc_offset);
                        return boost::mysql::field_view(boost::mysql::datetime(dt.year,
                                                                               dt.month,
                                                                               dt.day,
                                                                               dt.hour,
                                                                               dt.minute,
                                                                               dt.second,
                                                                               dt.microsecond));
                    }
                },
                p);
        }

        // Boost.MySQL 列类型 → 统一列类型。
        db::column_type
        map_column_type(boost::mysql::column_type t, bool u)
        {
            using b = boost::mysql::column_type;
            switch (t)
            {
                case b::tinyint:
                case b::smallint:
                case b::mediumint:
                case b::int_:
                case b::bigint:
                case b::year:
                    return u ? db::column_type::uint64 : db::column_type::int64;
                case b::bit:
                    return db::column_type::uint64;
                case b::float_:
                case b::double_:
                case b::decimal:
                    return db::column_type::double_;
                case b::varchar:
                case b::char_:
                case b::text:
                case b::enum_:
                case b::set:
                case b::json:
                    return db::column_type::string;
                case b::blob:
                case b::geometry:
                    return db::column_type::blob;
                case b::date:
                    return db::column_type::date;
                case b::datetime:
                    return db::column_type::datetime;
                case b::timestamp:
                    return db::column_type::timestamp;
                case b::time:
                    return db::column_type::time;
                default:
                    return db::column_type::unknown;
            }
        }

        // Boost.MySQL 字段视图 → 拥有型 db::field。
        // 注意：MySQL DECIMAL（如 SUM()/AVG() 的返回）以字符串传回，
        // 按列类型还原为数值，避免聚合结果无法用 as_int64/as_double 读取。
        field
        to_field(boost::mysql::field_view const& f, db::column_type ct, std::chrono::seconds utc_offset)
        {
            if (f.is_null())
            {
                return std::monostate {};
            }
            if (f.is_int64())
            {
                return f.as_int64();
            }
            if (f.is_uint64())
            {
                return f.as_uint64();
            }
            if (f.is_float())
            {
                return static_cast<double>(f.as_float());
            }
            if (f.is_double())
            {
                return f.as_double();
            }
            if (f.is_string())
            {
                std::string_view sv = f.as_string();
                if (ct == db::column_type::double_)
                {
                    // 整数形式（无小数点/指数）优先解析为 int64，保留精度。
                    // 注意：sv 不保证 null 结尾，须用 from_chars。
                    if (!sv.empty() && sv.find('.') == std::string_view::npos && sv.find('e') == std::string_view::npos
                        && sv.find('E') == std::string_view::npos)
                    {
                        int64_t ll = 0;
                        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), ll);
                        if (ec == std::errc {} && ptr == sv.data() + sv.size())
                        {
                            return static_cast<int64_t>(ll);
                        }
                        uint64_t ull = 0;
                        auto [ptr2, ec2] = std::from_chars(sv.data(), sv.data() + sv.size(), ull);
                        if (ec2 == std::errc {} && ptr2 == sv.data() + sv.size())
                        {
                            return static_cast<uint64_t>(ull);
                        }
                    }
                    double d = 0;
                    auto [ptr3, ec3] = std::from_chars(sv.data(), sv.data() + sv.size(), d);
                    if (ec3 == std::errc {} && ptr3 == sv.data() + sv.size())
                    {
                        return d;
                    }
                    throw db_exception(boost::system::error_code {}, "db: cannot parse DECIMAL value: " + std::string(sv));
                }
                return std::string(sv);
            }
            if (f.is_blob())
            {
                auto b = f.as_blob();
                return std::vector<std::byte>(reinterpret_cast<std::byte const*>(b.data()),
                                              reinterpret_cast<std::byte const*>(b.data()) + b.size());
            }
            if (f.is_date())
            {
                auto d = f.as_date();
                return date { d.year(), d.month(), d.day() };
            }
            if (f.is_datetime())
            {
                auto d = f.as_datetime();
                datetime dt { d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(), d.microsecond() };
                if (ct == db::column_type::timestamp)
                {
                    // TIMESTAMP 列：服务器存取时做会话时区换算，读回的是会话时区墙上时钟，
                    // 减偏移还原成 UTC 时间点；与 DATETIME 列（datetime 墙上时钟）区分，不混用。
                    return dt.to_time_point() - utc_offset;
                }
                return dt;
            }
            if (f.is_time())
            {
                return time::from_duration(std::chrono::duration_cast<std::chrono::microseconds>(f.as_time()));
            }
            return std::monostate {};
        }

        // boost::mysql::results → db::result。
        result
        build_result(boost::mysql::results&& data, std::chrono::seconds utc_offset)
        {
            std::vector<result::resultset> sets;
            if (data.has_value())
            {
                for (size_t rs_idx = 0; rs_idx < data.size(); ++rs_idx)
                {
                    auto rs = data[rs_idx];
                    result::resultset s;
                    s.affected = rs.affected_rows();
                    s.last_insert_id = rs.last_insert_id();

                    if (rs.has_value())
                    {
                        auto m = rs.meta();
                        s.names.reserve(m.size());
                        s.types.reserve(m.size());
                        for (auto& c : m)
                        {
                            auto name = c.column_name();
                            s.names.emplace_back(name.data(), name.size());
                            s.types.push_back(map_column_type(c.type(), c.is_unsigned()));
                        }

                        auto rows = rs.rows();
                        s.rows.reserve(rows.size());
                        for (auto rv : rows)
                        {
                            std::vector<field> values;
                            values.reserve(rv.size());
                            for (size_t ci = 0; ci < rv.size(); ++ci)
                            {
                                values.push_back(to_field(rv[ci], s.types[ci], utc_offset));
                            }
                            s.rows.push_back(std::move(values));
                        }
                    }
                    sets.push_back(std::move(s));
                }
            }
            return result(std::move(sets));
        }
    } // namespace

    mysql_backend::mysql_backend(net::any_io_executor ex, mysql_config cfg) : cfg_(std::move(cfg))
    {
        conn_ = std::make_unique<boost::mysql::any_connection>(ex);
    }

    bool
    mysql_backend::is_connection_lost(boost::system::error_code ec, boost::mysql::diagnostics const& diag)
    {
        if (!ec)
        {
            return false;
        }
        if (!diag.server_message().empty())
        {
            return false;
        }
        if (ec == boost::mysql::client_errc::wrong_num_params)
        {
            return false;
        }
        // 取消/超时（operation_aborted）不是断连：连接对象仍可复用。
        if (ec == net::error::operation_aborted)
        {
            return false;
        }
        return true;
    }

    void
    mysql_backend::raise_error(boost::system::error_code ec,
                               boost::mysql::diagnostics const& diag,
                               std::string_view sql,
                               std::string_view params)
    {
        if (!ec)
        {
            return;
        }
        if (is_connection_lost(ec, diag))
        {
            live_ = false;
        }
        auto what
            = std::string("[") + std::to_string(ec.value()) + "] " + ec.message() + " (SQL: " + std::string(sql) + ")";
        if (!params.empty())
        {
            what += " params: " + std::string(params);
        }
        auto msg = diag.server_message();
        if (!msg.empty())
        {
            what += ": " + std::string(msg.data(), msg.size());
        }
        throw db_exception(ec, what);
    }

    net::awaitable<void>
    mysql_backend::connect()
    {
        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(cfg_.host, cfg_.port);
        params.username = cfg_.user;
        params.password = cfg_.password;
        if (!cfg_.database.empty())
        {
            params.database = cfg_.database;
        }
        params.ssl = cfg_.ssl ? boost::mysql::ssl_mode::enable : boost::mysql::ssl_mode::disable;
        params.multi_queries = true;

        if (cfg_.connect_timeout.count() > 0)
        {
            co_await conn_->async_connect(params, net::cancel_after(cfg_.connect_timeout, net::use_awaitable));
        }
        else
        {
            co_await conn_->async_connect(params, net::use_awaitable);
        }

        conn_->set_meta_mode(boost::mysql::metadata_mode::full);

        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;

        if (!cfg_.charset.empty())
        {
            co_await conn_->async_execute("SET NAMES '" + cfg_.charset + "'",
                                          r,
                                          diag,
                                          net::redirect_error(net::use_awaitable, ec));
            raise_error(ec, diag);
        }

        if (!cfg_.time_zone.empty())
        {
            co_await conn_->async_execute("SET time_zone = '" + cfg_.time_zone + "'",
                                          r,
                                          diag,
                                          net::redirect_error(net::use_awaitable, ec));
            raise_error(ec, diag);
        }

        co_await conn_->async_execute("SELECT TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), NOW())",
                                      r,
                                      diag,
                                      net::redirect_error(net::use_awaitable, ec));
        raise_error(ec, diag);
        if (r.has_value() && !r.rows().empty())
        {
            auto f = r.rows()[0][0];
            if (f.is_int64())
            {
                utc_offset_ = std::chrono::seconds(f.as_int64());
            }
            else if (f.is_uint64())
            {
                utc_offset_ = std::chrono::seconds(static_cast<int64_t>(f.as_uint64()));
            }
        }
        co_return;
    }

    net::awaitable<void>
    mysql_backend::reconnect()
    {
        if (conn_)
        {
            boost::system::error_code ec;
            co_await conn_->async_close(net::redirect_error(net::use_awaitable, ec));
        }
        // 连接断开后服务器端 prepared statement 全部失效，统一层会先清空语句缓存。
        live_ = true;
        co_await connect();
    }

    net::awaitable<bool>
    mysql_backend::ping()
    {
        try
        {
            boost::system::error_code ec;
            // 带超时探测：网络分区（挂死而非拒绝）时避免 ping 无限阻塞连接池的维护协程。
            // 超时被 cancel 后 ec 置位，走下方置失效路径（与连接失败一致）。
            if (cfg_.ping_timeout.count() > 0)
            {
                co_await conn_->async_ping(net::redirect_error(
                    net::cancel_after(cfg_.ping_timeout, net::use_awaitable),
                    ec));
            }
            else
            {
                co_await conn_->async_ping(net::redirect_error(net::use_awaitable, ec));
            }
            if (ec)
            {
                live_ = false;
                co_return false;
            }
            co_return true;
        }
        catch (...)
        {
            live_ = false;
            co_return false;
        }
    }

    net::awaitable<result>
    mysql_backend::execute(std::string_view sql)
    {
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        boost::mysql::results data;
        co_await conn_->async_execute(sql, data, diag, net::redirect_error(net::use_awaitable, ec));
        raise_error(ec, diag, sql);

        co_return build_result(std::move(data), utc_offset_);
    }

    net::awaitable<statement_handle>
    mysql_backend::prepare(std::string_view sql)
    {
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        auto stmt = co_await conn_->async_prepare_statement(sql, diag, net::redirect_error(net::use_awaitable, ec));
        raise_error(ec, diag, sql);
        co_return statement_handle { std::make_shared<boost::mysql::statement>(std::move(stmt)) };
    }

    net::awaitable<result>
    mysql_backend::execute_statement(statement_handle h, std::vector<param> const& params)
    {
        auto* stmt = static_cast<boost::mysql::statement*>(h.state.get());
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;

        std::vector<boost::mysql::field_view> views;
        views.reserve(params.size());
        for (auto const& p : params)
        {
            views.push_back(to_field_view(p, utc_offset_));
        }

        boost::mysql::results data;
        if (views.empty())
        {
            co_await conn_->async_execute(stmt->bind(), data, diag, net::redirect_error(net::use_awaitable, ec));
        }
        else
        {
            co_await conn_->async_execute(stmt->bind(views.begin(), views.end()),
                                          data,
                                          diag,
                                          net::redirect_error(net::use_awaitable, ec));
        }
        raise_error(ec, diag);

        co_return build_result(std::move(data), utc_offset_);
    }

    net::awaitable<void>
    mysql_backend::close_statement(statement_handle h) noexcept
    {
        auto* stmt = static_cast<boost::mysql::statement*>(h.state.get());
        if (stmt && stmt->valid() && conn_)
        {
            boost::system::error_code close_ec;
            boost::mysql::diagnostics close_diag;
            try
            {
                co_await conn_->async_close_statement(*stmt,
                                                      close_diag,
                                                      net::redirect_error(net::use_awaitable, close_ec));
            }
            catch (...)
            {
                // 语句关闭不支持（老服务器）等本地错误：不影响连接可用性。
            }
            // close 失败不标记连接失效：重连前 / 失效后的清理路径 close 必然失败，属预期；
            // 连接健康由 ping/execute 等真实操作判定，避免因良性 close 错误误杀正常连接。
        }
        h.state.reset();
    }

    net::awaitable<void>
    mysql_backend::begin()
    {
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await conn_->async_execute("START TRANSACTION", r, diag, net::redirect_error(net::use_awaitable, ec));
        raise_error(ec, diag);
        co_return;
    }

    net::awaitable<void>
    mysql_backend::commit()
    {
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await conn_->async_execute("COMMIT", r, diag, net::redirect_error(net::use_awaitable, ec));
        raise_error(ec, diag);
        co_return;
    }

    net::awaitable<void>
    mysql_backend::rollback()
    {
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await conn_->async_execute("ROLLBACK", r, diag, net::redirect_error(net::use_awaitable, ec));
        raise_error(ec, diag);
        co_return;
    }

    void
    register_mysql_backend()
    {
        register_backend("mysql",
                         [](net::any_io_executor ex, options const& opts) -> std::unique_ptr<backend>
                         {
                             mysql_config cfg;
                             cfg.host = opts.get_or("host", cfg.host);
                             cfg.port = opts.as_uint16("port").value_or(cfg.port);
                             cfg.user = opts.get_or("user", cfg.user);
                             cfg.password = opts.get_or("password", cfg.password);
                             cfg.database = opts.get_or("db", opts.get_or("database", cfg.database));
                             cfg.charset = opts.get_or("charset", cfg.charset);
                             cfg.time_zone = opts.get_or("time_zone", cfg.time_zone);
                              cfg.connect_timeout = opts.as_seconds("connect_timeout").value_or(cfg.connect_timeout);
                              cfg.ping_timeout = opts.as_seconds("ping_timeout").value_or(cfg.ping_timeout);
                              cfg.ssl = opts.as_bool("ssl").value_or(cfg.ssl);
                             return std::make_unique<mysql_backend>(ex, std::move(cfg));
                         });
    }

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
