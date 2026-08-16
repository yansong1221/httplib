#pragma once

#include "httplib/config.hpp"
#include "httplib/db/binder.hpp"
#include "httplib/db/result.hpp"
#include <boost/asio/awaitable.hpp>
#include <string_view>
#include <vector>

namespace httplib::db::detail
{

    /**
     * \brief 数据库后端接口（每条连接一个实例）。
     * \details
     * 统一层（session / prepared_statement）只与本接口交互，不感知具体数据库。
     * \n
     * - \ref execute(sql, params) 内部负责 prepared statement 缓存与参数绑定；
     * - time_point 参数的时区换算由各后端自行消化（MySQL 用会话 UTC 偏移，SQLite 视为 UTC）。
     * \n
     * 接口用协程签名：MySQL 后端内部 `co_await` 网络操作；SQLite 后端内部同步执行后 `co_return`
     * （接受阻塞当前 io 线程，SQLite 查询极快）。
     */
    struct backend
    {
        virtual ~backend() = default;

        /// 建立连接 / 打开数据库文件。
        virtual net::awaitable<void> connect() = 0;

        /// 断开并重新建立连接（保留原配置）。默认等价于 connect()。
        virtual net::awaitable<void> reconnect() { co_await connect(); }

        /// 底层连接当前是否可用（出错后由各后端标记）。
        virtual bool alive() const { return true; }

        /// 探测存活；失败返回 false 并标记连接失效。
        virtual net::awaitable<bool> ping() = 0;

        /// 执行纯文本 SQL（无参数）。
        virtual net::awaitable<result> execute(std::string_view sql) = 0;

        /// 执行带参数 SQL（参数按占位符出现顺序排列）。
        /// cacheable=false 表示占位符数量随参数变化（数组展开），后端不得缓存该语句。
        virtual net::awaitable<result> execute(std::string_view sql,
                                               std::vector<param> const& params,
                                               bool cacheable = true) = 0;

        virtual net::awaitable<void> begin() = 0;
        virtual net::awaitable<void> commit() = 0;
        virtual net::awaitable<void> rollback() = 0;
    };

} // namespace httplib::db::detail
