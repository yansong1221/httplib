#pragma once

#include "httplib/config.hpp"
#include "httplib/db/binder.hpp"
#include "httplib/db/result.hpp"
#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::db::detail
{

    /// 后端 prepared statement 句柄（不透明）。由统一层（session）缓存并回传给后端执行/关闭。
    struct statement_handle
    {
        void* state = nullptr; ///< 后端特定句柄（分配资源，需 close_statement 释放）。
    };

    /**
     * \brief 数据库后端接口（每条连接一个实例）。
     * \details
     * 统一层（session / prepared_statement）只与本接口交互，不感知具体数据库。
     * \n
     * - 参数化语句统一由 session 层 prepare → 缓存 / 执行 → close，后端只提供原语：
     *   \ref prepare() 服务器端 prepare，\ref execute_statement() 绑定并执行，\ref close_statement() 释放；
     * - 占位符文本由 \ref placeholder() 提供，渲染层只维护编号、按序号向后端取值；
     * - time_point 参数的时区换算由各后端自行消化（MySQL 用会话 UTC 偏移，SQLite 视为 UTC）。
     * \n
     * 接口用协程签名：MySQL 后端内部 `co_await` 网络操作；SQLite 后端内部同步执行后 `co_return`
     * （接受阻塞当前 io 线程，SQLite 查询极快）。
     */
    struct backend
    {
        virtual ~backend() = default;

        /// 第 index 个（0 起）绑定占位符的文本；name 为参数名（位置绑定时为空串）。
        /// 默认 `?`（MySQL / SQLite）；PostgreSQL 用 `$N`，命名风格后端可直接用 name。
        virtual std::string
        placeholder(size_t index, std::string_view name) const
        {
            (void)index;
            (void)name;
            return "?";
        }

        /// 建立连接 / 打开数据库文件。
        virtual net::awaitable<void> connect() = 0;

        /// 断开并重新建立连接（保留原配置）。默认等价于 connect()。
        virtual net::awaitable<void>
        reconnect()
        {
            co_await connect();
        }

        /// 底层连接当前是否可用（出错后由各后端标记）。
        virtual bool
        alive() const
        {
            return true;
        }

        /// 探测存活；失败返回 false 并标记连接失效。
        virtual net::awaitable<bool> ping() = 0;

        /// 执行纯文本 SQL（无参数）。
        virtual net::awaitable<result> execute(std::string_view sql) = 0;

        /// 服务器端 prepare 一条语句，返回句柄（由统一层缓存，最终必须 close_statement）。
        virtual net::awaitable<statement_handle> prepare(std::string_view sql) = 0;

        /// 绑定参数并执行已 prepare 的语句（参数按占位符出现顺序排列）。
        virtual net::awaitable<result> execute_statement(statement_handle h, std::vector<param> const& params) = 0;

        /// 释放语句句柄（缓存逐出 / 连接重置时）。不得抛异常，错误由后端内部消化。
        virtual net::awaitable<void> close_statement(statement_handle h) noexcept = 0;

        virtual net::awaitable<void> begin() = 0;
        virtual net::awaitable<void> commit() = 0;
        virtual net::awaitable<void> rollback() = 0;
    };

} // namespace httplib::db::detail
