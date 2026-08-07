#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/config.hpp"
#include "httplib/db/db_connection.hpp"
#include "httplib/db/db_connection_pool.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server_fwd.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace httplib::server::middleware
{

    struct query_log_entry
    {
        std::string sql;
        std::chrono::microseconds duration {0};
        size_t row_count = 0;
        uint64_t affected_rows = 0;
        bool is_parameterized = false;
    };

    struct query_log_options
    {
        using log_callback
            = std::function<void(const request& req, const std::vector<query_log_entry>& entries)>;
        log_callback on_request_complete;

        std::chrono::microseconds slow_query_threshold {0};

        using slow_query_callback = std::function<void(const query_log_entry& entry)>;
        slow_query_callback on_slow_query;
    };

    class HTTPLIB_API db_query_log_middleware
    {
      public:
        explicit db_query_log_middleware(query_log_options opts = {});
        ~db_query_log_middleware();
        db_query_log_middleware(db_query_log_middleware const&);
        db_query_log_middleware& operator=(db_query_log_middleware const&);
        db_query_log_middleware(db_query_log_middleware&&) noexcept;
        db_query_log_middleware& operator=(db_query_log_middleware&&) noexcept;

        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
#endif // HTTPLIB_ENABLED_DATABASE
