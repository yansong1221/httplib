#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "backend.hpp"
#include "httplib/db/options.hpp"
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::db::detail
{

    /// 后端工厂：按 executor + 连接选项创建一条 backend。
    using backend_factory = std::function<std::unique_ptr<backend>(net::any_io_executor ex, options const& opts)>;

    /// 注册一个后端；同名重复注册返回 false。
    /// HTTPLIB_API：内部细节，导出仅供测试注入假后端。
    HTTPLIB_API bool register_backend(std::string_view name, backend_factory factory);

    /// 查找后端工厂；未注册返回 nullptr。
    backend_factory const* find_backend(std::string_view name);

    /// 已注册后端名列表（用于错误提示）。
    std::string registered_backend_names();

    /// 注册编译进库的默认后端（幂等）。新增后端只需在此登记并提供注册函数。
    void register_backends();

    // 各后端提供的注册函数（在各自的实现文件里定义）。
    void register_mysql_backend();
    void register_sqlite_backend();
    void register_odbc_backend();

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
