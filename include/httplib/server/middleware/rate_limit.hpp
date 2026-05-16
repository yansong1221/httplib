#pragma once
#include "httplib/config.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <chrono>
#include <cstdint>
#include <memory>

namespace httplib::server::middleware {

class HTTPLIB_API rate_limit_middleware
{
public:
    rate_limit_middleware(uint32_t max_requests, std::chrono::steady_clock::duration window);
    ~rate_limit_middleware();

    bool before(request& req, response& resp);

private:
    class impl;
    std::shared_ptr<impl> impl_;
};

} // namespace httplib::server::middleware
