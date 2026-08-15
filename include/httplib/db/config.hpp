#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace httplib::db
{

    /**
     * \brief MySQL 连接配置。
     */
    struct mysql_config
    {
        std::string host = "127.0.0.1";
        uint16_t port = 3306;
        std::string user;
        std::string password;
        /// 连接后默认使用的数据库（可为空）。
        std::string database;
        /// 连接字符集（连接后执行 `SET NAMES`）。
        std::string charset = "utf8mb4";
        /// 会话时区（连接后执行 `SET time_zone`），固定偏移如 `"+00:00"`。
        std::string time_zone = "+00:00";
        /// 连接超时（0 表示不设超时）。
        std::chrono::seconds connect_timeout { 5 };
        /// 是否使用 SSL。
        bool ssl = false;
        /// prepared statement 缓存上限（0 表示不缓存）。
        size_t max_cached_statements = 64;
    };

    /**
     * \brief SQLite 连接配置。
     */
    struct sqlite_config
    {
        /// 数据库文件路径；":memory:" 表示内存库。
        std::string path = ":memory:";
    };

} // namespace httplib::db
