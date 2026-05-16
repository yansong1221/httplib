#pragma once
#include "httplib/server/middleware/session.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace httplib::server::middleware {

class HTTPLIB_API memory_session_store : public session_store
{
public:
    explicit memory_session_store(std::chrono::seconds ttl = std::chrono::hours(24));
    ~memory_session_store() override;

    std::shared_ptr<session> load(std::string_view id) override;
    void save(const session& s) override;
    void destroy(std::string_view id) override;
    void cleanup();

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::server::middleware
