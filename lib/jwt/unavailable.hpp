#pragma once
/// \brief 未启用 JWT 构建的失效桩助手（只被各实现文件的 `#else` 段包含）。
///
/// 失效桩分散在多个实现文件里，而 unity build 会把它们合并成同一个翻译单元；
/// 若各文件在匿名命名空间各写一份，就会出现重定义（C2084）。这里给出唯一定义：
///
/// - unavailable()          非 noexcept 入口：抛 protocol_not_supported；
/// - unavailable_noexcept() noexcept 入口：std::abort。
///
/// 启用构建从不包含本头，真实现不引入任何额外依赖或开销。

#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <cstdlib>

namespace httplib::jwt::detail
{
    /// \brief 以 protocol_not_supported 报告「库未启用该能力」。
    [[noreturn]] inline void
    unavailable()
    {
        throw boost::system::system_error(
            boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
    }

    /// \brief noexcept 入口的兜底：无可报告的错误通道，直接终止。
    [[noreturn]] inline void
    unavailable_noexcept() noexcept
    {
        std::abort();
    }
} // namespace httplib::jwt::detail
