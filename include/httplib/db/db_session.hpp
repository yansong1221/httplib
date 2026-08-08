#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/db/db_result.hpp"
#include "httplib/db/prepared_statement.hpp"
#include "httplib/db/transaction.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::db
{

    struct query_log_entry
    {
        std::string sql;
        std::chrono::microseconds duration { 0 };
        size_t row_count = 0;
        uint64_t affected_rows = 0;
        bool is_parameterized = false;
    };

    class HTTPLIB_API db_session
    {
      public:
        using query_logger = std::function<void(query_log_entry const&)>;

        db_session() = delete;
        db_session(db_session const&) = delete;
        db_session& operator=(db_session const&) = delete;
        db_session(db_session&&) noexcept;
        db_session& operator=(db_session&&) noexcept;
        ~db_session();

        net::awaitable<db_result> query(std::string_view sql);

        prepared_statement stmt(std::string_view sql);

        net::awaitable<transaction> begin();

        net::awaitable<bool> ping();

        void set_query_logger(query_logger cb);

        struct impl;
        explicit db_session(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;

        friend impl& get_impl(db_session& self);
        friend impl const& get_impl(db_session const& self);
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
