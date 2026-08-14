#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace httplib::mysql
{

    /**
     * \brief 建立 MySQL 连接的参数。
     * \details
     * 用于 \ref session::connect 以及 \ref connection_pool 的配置。
     * \n
     * 所有字段都有合理的默认值，通常只需要设置 user / password（以及可选的 database）。
     */
    struct connect_params
    {
        /// 服务器主机名或 IP 地址。
        std::string host = "127.0.0.1";
        /// 服务器端口。
        uint16_t port = 3306;
        /// 用户名。
        std::string user;
        /// 密码。
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
     * \brief 连接池参数。
     * \details
     * 继承自 \ref connect_params，并添加池的大小与维护相关配置。
     */
    struct pool_params : connect_params
    {
        /// 池维持的最小空闲连接数。
        size_t min_connections = 2;
        /// 池允许的最大连接数（空闲 + 活跃）。
        size_t max_connections = 16;

        /// 取连接时的默认等待超时（池满时）。
        std::chrono::seconds acquire_timeout { 5 };
        /// 空闲连接超过该时长即被回收。
        std::chrono::seconds idle_timeout { 300 };
        /// 空闲检查 / 回收的周期。
        std::chrono::seconds idle_check_interval { 60 };
        /// 空闲连接健康检查（ping）的周期。
        std::chrono::seconds health_check_interval { 30 };
        /// 借出连接前是否 ping 校验（失效则丢弃重建）。默认关闭，本地部署可开启。
        bool validate_on_borrow = false;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
