#ifdef HTTPLIB_ENABLED_DATABASE
#    include "httplib/db/db_session.hpp"
#    include "db/db_result_impl.h"
#    include "db/db_session_impl.h"
#    include "httplib/db/db_pool.hpp"
#    include <boost/asio/redirect_error.hpp>
#    include <boost/asio/use_awaitable.hpp>
#    include <boost/mysql.hpp>
#    include <stdexcept>

namespace httplib::db
{

    db_session::db_session(std::unique_ptr<impl> p) : impl_(std::move(p)) {}

    db_session::db_session(db_session&&) noexcept = default;
    db_session& db_session::operator=(db_session&&) noexcept = default;
    db_session::~db_session() = default;

    db_session::impl&
    get_impl(db_session& self)
    {
        return *self.impl_;
    }

    db_session::impl const&
    get_impl(db_session const& self)
    {
        return *self.impl_;
    }

    net::awaitable<db_result>
    db_session::query(std::string_view sql)
    {
        co_return co_await get_impl(*this).query_raw(sql, {});
    }

    prepared_statement
    db_session::stmt(std::string_view sql)
    {
        return prepared_statement(*this, std::string(sql));
    }

    net::awaitable<bool>
    db_session::ping()
    {
        try
        {
            co_await get_impl(*this).pooled.get().async_ping(boost::asio::use_awaitable);
            co_return true;
        }
        catch (...)
        {
            co_return false;
        }
    }

    void
    db_session::set_query_logger(query_logger cb)
    {
        get_impl(*this).query_logger = std::move(cb);
    }

    net::awaitable<db_result>
    db_session::impl::query_raw(std::string_view sql, std::span<boost::mysql::field_view const> params)
    {
        auto start = std::chrono::steady_clock::now();

        auto result_impl = std::make_unique<db_result::impl>();

        pooled.get().set_meta_mode(boost::mysql::metadata_mode::full);

        if (params.empty())
        {
            co_await pooled.get().async_execute(std::string(sql), result_impl->data, boost::asio::use_awaitable);
        }
        else
        {
            auto stmt = co_await pooled.get().async_prepare_statement(std::string(sql), boost::asio::use_awaitable);

            co_await pooled.get().async_execute(stmt.bind(params.begin(), params.end()),
                                                result_impl->data,
                                                boost::asio::use_awaitable);

            boost::system::error_code ec;
            co_await pooled.get().async_close_statement(stmt,
                                                        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        }

        build_result_impl(*result_impl);
        auto result = db_result(std::move(result_impl));

        if (query_logger)
        {
            query_log_entry entry;
            entry.sql = std::string(sql);
            entry.duration
                = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            entry.row_count = result.row_count();
            entry.affected_rows = result.affected_rows();
            entry.is_parameterized = !params.empty();
            query_logger(entry);
        }

        co_return result;
    }

    net::awaitable<void>
    db_session::impl::begin_transaction()
    {
        boost::mysql::results r;
        co_await pooled.get().async_execute("START TRANSACTION", r, boost::asio::use_awaitable);
        in_transaction = true;
    }

    net::awaitable<void>
    db_session::impl::commit()
    {
        boost::mysql::results r;
        co_await pooled.get().async_execute("COMMIT", r, boost::asio::use_awaitable);
        in_transaction = false;
    }

    net::awaitable<void>
    db_session::impl::rollback()
    {
        boost::mysql::results r;
        co_await pooled.get().async_execute("ROLLBACK", r, boost::asio::use_awaitable);
        in_transaction = false;
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
