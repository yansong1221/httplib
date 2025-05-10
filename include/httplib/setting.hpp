#pragma once
#include "httplib/server.hpp"

namespace httplib {

struct server::setting
{
    struct SSLConfig
    {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    std::optional<SSLConfig> ssl_conf;
    std::chrono::steady_clock::duration read_timeout  = std::chrono::seconds(30);
    std::chrono::steady_clock::duration write_timeout = std::chrono::seconds(30);

    websocket_conn::message_handler_type websocket_message_handler;
    websocket_conn::open_handler_type websocket_open_handler;
    websocket_conn::close_handler_type websocket_close_handler;

public:
    setting();

public:
    std::shared_ptr<spdlog::logger> get_logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

private:
    std::shared_ptr<spdlog::logger> default_logger_;
    std::shared_ptr<spdlog::logger> custom_logger_;
};

} // namespace httplib