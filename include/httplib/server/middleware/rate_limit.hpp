#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <chrono>
#include <cstdint>
#include <memory>

namespace httplib::server::middleware
{

    class HTTPLIB_API rate_limit
    {
      public:
        rate_limit(uint32_t max_requests, std::chrono::steady_clock::duration window);
        ~rate_limit();

        bool before(request& req, response& resp);

      private:
        class impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
