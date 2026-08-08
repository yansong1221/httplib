#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#    include "httplib/config.hpp"
#    include "httplib/db/db_fwd.hpp"
#    include "httplib/db/db_result.hpp"
#    include <cstdint>
#    include <memory>
#    include <string>
#    include <string_view>

namespace httplib::db
{

    class HTTPLIB_API prepared_statement
    {
      public:
        prepared_statement(prepared_statement&&) noexcept;
        prepared_statement& operator=(prepared_statement&&) noexcept;
        ~prepared_statement();

        prepared_statement(prepared_statement const&) = delete;
        prepared_statement& operator=(prepared_statement const&) = delete;

        prepared_statement& bind(std::string_view v);
        prepared_statement& bind(std::string const& v);
        prepared_statement& bind(char const* v);
        prepared_statement& bind(int64_t v);
        prepared_statement& bind(uint64_t v);
        prepared_statement& bind(int v);
        prepared_statement& bind(unsigned v);
        prepared_statement& bind(short v);
        prepared_statement& bind(unsigned short v);
        prepared_statement& bind(double v);
        prepared_statement& bind(float v);
        prepared_statement& bind(bool v);
        prepared_statement& bind(std::nullptr_t);
        prepared_statement& bind(db_date v);
        prepared_statement& bind(db_datetime v);
        prepared_statement& bind(std::chrono::microseconds v);
        prepared_statement& bind_timestamp(int64_t epoch);
        prepared_statement& bind_timestamp(std::string_view name, int64_t epoch);

        prepared_statement& bind(std::string_view name, std::string_view v);
        prepared_statement& bind(std::string_view name, char const* v);
        prepared_statement& bind(std::string_view name, int64_t v);
        prepared_statement& bind(std::string_view name, uint64_t v);
        prepared_statement& bind(std::string_view name, int v);
        prepared_statement& bind(std::string_view name, unsigned v);
        prepared_statement& bind(std::string_view name, short v);
        prepared_statement& bind(std::string_view name, unsigned short v);
        prepared_statement& bind(std::string_view name, double v);
        prepared_statement& bind(std::string_view name, float v);
        prepared_statement& bind(std::string_view name, bool v);
        prepared_statement& bind(std::string_view name, std::nullptr_t);
        prepared_statement& bind(std::string_view name, db_date v);
        prepared_statement& bind(std::string_view name, db_datetime v);
        prepared_statement& bind(std::string_view name, std::chrono::microseconds v);

        net::awaitable<db_result> execute();

        prepared_statement& into(int64_t& v, size_t col);
        prepared_statement& into(uint64_t& v, size_t col);
        prepared_statement& into(double& v, size_t col);
        prepared_statement& into(float& v, size_t col);
        prepared_statement& into(bool& v, size_t col);
        prepared_statement& into(std::string& v, size_t col);

        prepared_statement& into(int64_t& v, std::string_view name);
        prepared_statement& into(uint64_t& v, std::string_view name);
        prepared_statement& into(double& v, std::string_view name);
        prepared_statement& into(float& v, std::string_view name);
        prepared_statement& into(bool& v, std::string_view name);
        prepared_statement& into(std::string& v, std::string_view name);

        struct impl;
        explicit prepared_statement(db_session& sess, std::string sql);

      private:
        std::unique_ptr<impl> impl_;
    };
} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
