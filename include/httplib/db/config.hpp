#pragma once

#include "options.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace httplib::db
{

    /**
     * \brief MySQL 连接配置（`"mysql"` 后端）。
     * \details
     * 既可作为类型化配置传给 \ref session::connect(ex, mysql_config)，
     * 也可等价地用连接串 `connect(ex, "mysql", "...")` / \ref make_pool，连接串支持的键如下：
     * \n
     * - `host`：服务器地址，默认 `127.0.0.1`；
     * - `port`：端口，默认 `3306`；
     * - `user`：用户名，默认空；
     * - `password`：密码，默认空；
     * - `db` / `database`：连接后默认使用的数据库（可为空），默认空；
     * - `charset`：连接字符集（连接后执行 `SET NAMES`），默认 `utf8mb4`；
     * - `time_zone`：会话时区（连接后执行 `SET time_zone`），固定偏移如 `+08:00`；
     *   为空表示不覆盖，沿用服务器默认会话时区（与 mysql 客户端行为一致），默认空；
     * - `connect_timeout`：连接超时（秒），`0` 表示不设超时，默认 `5`；
     * - `ssl`：是否使用 SSL，`1/true/yes/on` 或 `0/false/no/off`，默认 `0`；
     * - `max_cached_statements`：prepared statement 缓存上限（由统一层 session 管理），`0` 表示不缓存，默认 `64`。
     * \n
     * 例：`"host=127.0.0.1 port=3306 user=root password=123456 db=main time_zone=+08:00"`
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
        /// 会话时区（连接后执行 `SET time_zone`），固定偏移如 `"+08:00"`。
        /// 为空表示不覆盖，沿用服务器默认会话时区（与 mysql 客户端行为一致）。
        std::string time_zone;
        /// 连接超时（0 表示不设超时）。
        std::chrono::seconds connect_timeout { 5 };
        /// 是否使用 SSL。
        bool ssl = false;
        /// prepared statement 缓存上限（0 表示不缓存）。
        size_t max_cached_statements = 64;

        /// 转成等价的后端连接选项（供字符串连接 API 复用）。
        options
        to_options() const
        {
            options o;
            o.set("host", host);
            o.set("port", std::to_string(port));
            o.set("user", user);
            o.set("password", password);
            o.set("db", database);
            o.set("charset", charset);
            o.set("time_zone", time_zone);
            o.set("connect_timeout", std::to_string(connect_timeout.count()));
            o.set("ssl", ssl ? "1" : "0");
            o.set("max_cached_statements", std::to_string(max_cached_statements));
            return o;
        }
    };

    /**
     * \brief SQLite 连接配置（`"sqlite"` 后端）。
     * \details
     * 既可作为类型化配置传给 \ref session::connect(ex, sqlite_config)，
     * 也可等价地用连接串 `connect(ex, "sqlite", "...")` / \ref make_pool，连接串支持的键如下：
     * \n
     * - `db` / `path`：数据库文件路径；`:memory:` 表示内存库，默认 `:memory:`。
     * \n
     * 例：`"db=./data.db"`
     */
    struct sqlite_config
    {
        /// 数据库文件路径；":memory:" 表示内存库。
        std::string path = ":memory:";

        /// 转成等价的后端连接选项（供字符串连接 API 复用）。
        options
        to_options() const
        {
            options o;
            o.set("db", path);
            return o;
        }
    };

    /**
     * \brief ODBC 连接配置（`"odbc"` 后端）。
     * \details
     * 既可作为类型化配置传给 \ref session::connect(ex, odbc_config)，
     * 也可等价地用连接串 `connect(ex, "odbc", "...")` / \ref make_pool，连接串支持的键如下：
     * \n
     * - `connection_string`：完整 ODBC 连接串（`Driver={...};Server=...;...`），默认空；
     * - `dsn`：数据源名（与 `connection_string` 二选一，优先用连接串），默认空；
     * - `uid`：用户名（DSN 方式时可选），默认空；
     * - `pwd`：密码（DSN 方式时可选），默认空；
     * - `max_cached_statements`：prepared statement 缓存上限（由统一层 session 管理），默认 `64`。
     * \n
     * 例（SQL Server）：`"connection_string=Driver={ODBC Driver 17 for SQL
     * Server};Server=localhost;Database=test;Trusted_Connection=yes;"`
     */
    struct odbc_config
    {
        /// 完整 ODBC 连接串（`Driver={...};Server=...;Database=...;...`）。
        std::string connection_string;
        /// DSN 数据源名（与 connection_string 二选一）。
        std::string dsn;
        /// DSN 方式登录用户名（可为空）。
        std::string user;
        /// DSN 方式登录密码（可为空）。
        std::string password;
        /// prepared statement 缓存上限（0 表示不缓存）。
        size_t max_cached_statements = 64;

        /// 转成等价的后端连接选项（供字符串连接 API 复用）。
        options
        to_options() const
        {
            options o;
            if (!connection_string.empty())
            {
                o.set("connection_string", connection_string);
            }
            if (!dsn.empty())
            {
                o.set("dsn", dsn);
            }
            if (!user.empty())
            {
                o.set("uid", user);
            }
            if (!password.empty())
            {
                o.set("pwd", password);
            }
            o.set("max_cached_statements", std::to_string(max_cached_statements));
            return o;
        }
    };

} // namespace httplib::db
