#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/config.hpp"
#include "httplib/mysql/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server_fwd.hpp"
#include <functional>
#include <memory>
#include <vector>

namespace httplib::server::middleware
{

    struct query_log_options
    {
        using log_callback = std::function<void(request const& req, std::vector<mysql::query_log_entry> const& entries)>;
        log_callback on_request_complete;

        std::chrono::microseconds slow_query_threshold { 0 };

        using slow_query_callback = std::function<void(mysql::query_log_entry const& entry)>;
        slow_query_callback on_slow_query;
    };

    class HTTPLIB_API mysql_query_log_middleware
    {
      public:
        explicit mysql_query_log_middleware(query_log_options opts = {});
        ~mysql_query_log_middleware();
        mysql_query_log_middleware(mysql_query_log_middleware const&);
        mysql_query_log_middleware& operator=(mysql_query_log_middleware const&);
        mysql_query_log_middleware(mysql_query_log_middleware&&) noexcept;
        mysql_query_log_middleware& operator=(mysql_query_log_middleware&&) noexcept;

        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
