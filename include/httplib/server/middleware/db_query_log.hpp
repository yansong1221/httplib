#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/config.hpp"
#include "httplib/db/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server_fwd.hpp"
#include <functional>
#include <memory>
#include <vector>

namespace httplib::server::middleware
{

    struct query_log_options
    {
        using value_type = std::vector<db::query_log_entry>;

        using log_callback = std::function<void(request const& req, value_type const& entries)>;
        log_callback on_request_complete;

        std::chrono::microseconds slow_query_threshold { 0 };

        using slow_query_callback = std::function<void(db::query_log_entry const& entry)>;
        slow_query_callback on_slow_query;
    };

    class HTTPLIB_API db_query_log_middleware
    {
      public:
        using value_type = std::shared_ptr<query_log_options::value_type>;

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
