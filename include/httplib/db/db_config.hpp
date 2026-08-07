#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include <chrono>
#include <cstdint>
#include <string>

namespace httplib::db
{

    struct db_config
    {
        std::string host = "127.0.0.1";
        uint16_t port = 3306;
        std::string user;
        std::string password;
        std::string database;
        std::string charset = "utf8mb4";

        size_t min_connections = 2;
        size_t max_connections = 16;

        std::chrono::seconds idle_timeout {300};
        std::chrono::seconds acquire_timeout {5};
        std::chrono::seconds query_timeout {30};
        bool auto_reconnect = true;

        std::chrono::seconds idle_check_interval {60};
        std::chrono::seconds health_check_interval {30};
        std::chrono::seconds ping_grace_period {15};

        size_t stmt_cache_size = 64;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
