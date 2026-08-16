#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "backend.hpp"
#include "httplib/db/config.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/mysql.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace httplib::db::detail
{

    /// MySQL 后端：持有 any_connection；语句缓存由统一层（session）管理。
    class mysql_backend : public backend
    {
      public:
        explicit mysql_backend(net::any_io_executor ex, mysql_config cfg);

        net::awaitable<void> connect() override;
        net::awaitable<void> reconnect() override;
        net::awaitable<bool> ping() override;
        bool
        alive() const override
        {
            return live_;
        }

        net::awaitable<result> execute(std::string_view sql) override;
        net::awaitable<statement_handle> prepare(std::string_view sql) override;
        net::awaitable<result> execute_statement(statement_handle h, std::vector<param> const& params) override;
        net::awaitable<void> close_statement(statement_handle h) noexcept override;

        net::awaitable<void> begin() override;
        net::awaitable<void> commit() override;
        net::awaitable<void> rollback() override;

        /// 会话相对 UTC 的偏移（秒）。
        std::chrono::seconds
        utc_offset() const
        {
            return utc_offset_;
        }

      private:
        void raise_error(boost::system::error_code ec,
                         boost::mysql::diagnostics const& diag,
                         std::string_view sql = {},
                         std::string_view params = {});
        static bool is_connection_lost(boost::system::error_code ec, boost::mysql::diagnostics const& diag);

        std::unique_ptr<boost::mysql::any_connection> conn_;
        mysql_config cfg_;
        bool live_ = true;
        std::chrono::seconds utc_offset_ { 0 };
    };

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
