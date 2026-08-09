#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/prepared_statement.hpp"
#include "httplib/mysql/result.hpp"
#include "httplib/mysql/transaction.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::mysql
{

    struct query_log_entry
    {
        std::string sql;
        std::chrono::steady_clock::duration duration { 0 };
        size_t row_count = 0;
        uint64_t affected_rows = 0;
        bool is_parameterized = false;
    };

    class HTTPLIB_API session
    {
      public:
        using query_logger = std::function<void(query_log_entry const&)>;

        session(session&&) noexcept;
        session& operator=(session&&) noexcept;
        ~session();

        net::awaitable<result> query(std::string_view sql);

        prepared_statement stmt(std::string_view sql);

        net::awaitable<transaction> begin();

        net::awaitable<bool> ping();

        void set_query_logger(query_logger cb);

        struct impl;
        explicit session(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;

        friend impl& get_impl(session& self);
        friend impl const& get_impl(session const& self);
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
