#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/mysql_fwd.hpp"
#include "httplib/mysql/result.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::mysql
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
        prepared_statement& bind(date v);
        prepared_statement& bind(datetime v);
        prepared_statement& bind(time v);
        prepared_statement& bind(net::const_buffer v);
        prepared_statement& bind(boost::json::value const& v);
        prepared_statement& bind(std::chrono::system_clock::time_point tp);

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
        prepared_statement& bind(std::string_view name, date v);
        prepared_statement& bind(std::string_view name, datetime v);
        prepared_statement& bind(std::string_view name, time v);
        prepared_statement& bind(std::string_view name, net::const_buffer v);
        prepared_statement& bind(std::string_view name, boost::json::value const& v);
        prepared_statement& bind(std::string_view name, std::chrono::system_clock::time_point tp);

        net::awaitable<result> execute();

        prepared_statement& into(int64_t& v, size_t col);
        prepared_statement& into(uint64_t& v, size_t col);
        prepared_statement& into(double& v, size_t col);
        prepared_statement& into(float& v, size_t col);
        prepared_statement& into(bool& v, size_t col);
        prepared_statement& into(std::string& v, size_t col);
        prepared_statement& into(date& v, size_t col);
        prepared_statement& into(datetime& v, size_t col);
        prepared_statement& into(time& v, size_t col);
        prepared_statement& into(std::chrono::system_clock::time_point& v, size_t col);
        prepared_statement& into(net::const_buffer& v, size_t col);
        prepared_statement& into(boost::json::value& v, size_t col);

        prepared_statement& into(int64_t& v, std::string_view name);
        prepared_statement& into(uint64_t& v, std::string_view name);
        prepared_statement& into(double& v, std::string_view name);
        prepared_statement& into(float& v, std::string_view name);
        prepared_statement& into(bool& v, std::string_view name);
        prepared_statement& into(std::string& v, std::string_view name);
        prepared_statement& into(date& v, std::string_view name);
        prepared_statement& into(datetime& v, std::string_view name);
        prepared_statement& into(time& v, std::string_view name);
        prepared_statement& into(std::chrono::system_clock::time_point& v, std::string_view name);
        prepared_statement& into(net::const_buffer& v, std::string_view name);
        prepared_statement& into(boost::json::value& v, std::string_view name);

        struct impl;
        explicit prepared_statement(session& sess, std::string sql);

      private:
        std::unique_ptr<impl> impl_;
    };
} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
