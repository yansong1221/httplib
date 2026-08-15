#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "backend.hpp"
#include "httplib/db/config.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/mysql.hpp>
#include <chrono>
#include <list>
#include <memory>
#include <optional>
#include <string>

namespace httplib::db::detail
{

    /// MySQL 后端：持有 any_connection 与 prepared statement LRU 缓存。
    class mysql_backend : public backend
    {
      public:
        explicit mysql_backend(net::any_io_executor ex, mysql_config cfg);

        net::awaitable<void> connect() override;
        net::awaitable<void> reconnect() override;
        net::awaitable<bool> ping() override;
        bool alive() const override { return live_; }

        net::awaitable<result> execute(std::string_view sql) override;
        net::awaitable<result> execute(std::string_view sql, std::vector<param> const& params) override;

        net::awaitable<void> begin() override;
        net::awaitable<void> commit() override;
        net::awaitable<void> rollback() override;

        /// 会话相对 UTC 的偏移（秒）。
        std::chrono::seconds utc_offset() const { return utc_offset_; }

      private:
        struct statement_cache
        {
            struct entry
            {
                boost::mysql::statement stmt;
                std::list<std::string>::iterator lru_it;
            };
            util::string_map<entry> map;
            std::list<std::string> lru;
            size_t capacity = 64;
        };

        void raise_error(boost::system::error_code ec,
                         boost::mysql::diagnostics const& diag,
                         std::string_view sql = {},
                         std::string_view params = {});
        static bool is_connection_lost(boost::system::error_code ec, boost::mysql::diagnostics const& diag);

        boost::mysql::statement* find_statement(std::string_view sql);
        std::optional<boost::mysql::statement> store_statement(std::string sql, boost::mysql::statement stmt);

        std::unique_ptr<boost::mysql::any_connection> conn_;
        mysql_config cfg_;
        bool live_ = true;
        std::chrono::seconds utc_offset_ { 0 };
        statement_cache stmt_cache_;
    };

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
