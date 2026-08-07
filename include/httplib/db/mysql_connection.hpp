#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/db/db_config.hpp"
#include "httplib/db/db_connection.hpp"
#include "httplib/db/db_connection_pool.hpp"
#include <boost/mysql.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace httplib::db
{

    class stmt_cache;

    class HTTPLIB_API mysql_connection : public db_connection
    {
      public:
        ~mysql_connection() override;

        [[nodiscard]] static net::awaitable<std::shared_ptr<mysql_connection>> create(net::any_io_executor ex,
                                                                                      const db_config& config);
        [[nodiscard]] static net::awaitable<std::shared_ptr<mysql_connection>> create(boost::asio::io_context& io_ctx,
                                                                                      const db_config& config);

        [[nodiscard]] static db_connection_factory
        make_factory();

        [[deprecated("Unsafe: use query(sql, params) to prevent SQL injection")]] [[nodiscard]] net::awaitable<
            db_result>
        query(std::string_view sql) override;
        [[nodiscard]] net::awaitable<db_result> query(std::string_view sql,
                                                     std::span<const std::string> params) override;

        [[deprecated("Unsafe: use execute(sql, params) to prevent SQL injection")]] [[nodiscard]] net::awaitable<
            db_result>
        execute(std::string_view sql) override;
        [[nodiscard]] net::awaitable<db_result> execute(std::string_view sql,
                                                       std::span<const std::string> params) override;

        net::awaitable<void> begin_transaction() override;
        net::awaitable<void> commit() override;
        net::awaitable<void> rollback() override;
        bool in_transaction() const override;

        bool is_alive() const override;
        [[nodiscard]] net::awaitable<bool> ping() override;

        [[nodiscard]] std::string_view backend() const override;
        [[nodiscard]] std::chrono::steady_clock::time_point last_active_time() const override;
        [[nodiscard]] std::chrono::steady_clock::time_point last_ping_time() const override;
        void touch() override;

      private:
        explicit mysql_connection(net::any_io_executor ex, size_t stmt_cache_size);

        net::awaitable<boost::mysql::statement>
        get_or_prepare(std::string_view sql);
        static db_result
        convert_results(const boost::mysql::results& boost_results);
        static void
        validate_charset(const std::string& charset);

        net::any_io_executor ex_;
        boost::mysql::any_connection conn_;
        std::unique_ptr<stmt_cache> stmt_cache_;
        bool alive_ = false;
        bool in_transaction_ = false;
        std::chrono::steady_clock::time_point last_active_;
        std::chrono::steady_clock::time_point last_ping_;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
