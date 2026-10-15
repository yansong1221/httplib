#pragma once
#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>
#include <type_traits>

namespace httplib::db
{

    /// \brief 数据库模块错误码。
    /// \details 只描述数据层自身的语义错误（SQL/参数绑定、结果集访问、会话状态、
    /// 连接池），让调用方按 \ref db_exception::code() 分流而不必匹配错误消息。
    /// \n
    /// 后端 / 驱动 / 系统层面的错误（MySQL 错误号、SQLite rc、ODBC SQLSTATE、文件 I/O）
    /// 不在此枚举内，仍按各后端的既有方式报告（大多通过 \ref db_exception::what()）。
    enum class error
    {
        /// 绑定参数与 SQL 不匹配：缺参、多参、位置与命名混用、命名参数未绑定，
        /// 或参数化语句不支持的多语句 SQL。
        invalid_parameters = 1,
        /// 结果集中不存在所请求的列名。
        column_not_found,
        /// 取到的字段无法转换为请求的 C++ 类型：类型不符、超出表示范围或文本解析失败。
        type_mismatch,
        /// 需要非 NULL 的位置取到了 NULL（如 \ref into(vector) / \ref into(scalar)）。
        null_value,
        /// 会话尚未连接，或语句尚未 prepare。
        not_initialized,
        /// 在 wait_timeout 内没有从连接池借到连接（含 fail-fast）。
        acquire_timeout,
        /// 请求了未注册的后端名。
        unknown_backend,
    };

    HTTPLIB_API boost::system::error_code make_error_code(error e);

} // namespace httplib::db

namespace boost::system
{
    /// 让 `error` 可直接隐式转换为 `boost::system::error_code`
    /// （如 `boost::system::error_code ec = httplib::db::error::type_mismatch;`），
    /// 与 httplib::client::error 的注册方式保持一致。
    template <>
    struct is_error_code_enum<httplib::db::error> : std::true_type
    {
    };
} // namespace boost::system
