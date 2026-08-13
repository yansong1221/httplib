#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace httplib::mysql
{

    struct connect_params
    {
        std::string host = "127.0.0.1";
        uint16_t port = 3306;
        std::string user;
        std::string password;
        std::string database;
        std::string charset = "utf8mb4";
        std::string time_zone = "+00:00";
        std::chrono::seconds connect_timeout { 5 };
        bool ssl = false;
        size_t max_cached_statements = 64;
    };

    struct pool_params : connect_params
    {
        size_t min_connections = 2;
        size_t max_connections = 16;

        std::chrono::seconds acquire_timeout { 5 };
        std::chrono::seconds idle_timeout { 300 };
        std::chrono::seconds idle_check_interval { 60 };
        std::chrono::seconds health_check_interval { 30 };
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
