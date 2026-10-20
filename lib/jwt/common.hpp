#pragma once
#include "httplib/config.hpp"
/// \brief JWT 实现内部共享声明（公共头不含本文件）。
///
/// 本文件恒定编译，且不含任何 jwt-cpp 依赖：只放与引擎无关的内部声明，
/// 供常编 TU（error.cpp —— 纯错误类别注册）使用。
///
/// 引擎侧内部件（jwt-cpp 别名、base64url 解码、algorithm 桥）见 engine.hpp，
/// 它只被 `HTTPLIB_ENABLED_JWT` 打开的实现段包含。

#include <boost/system/error_code.hpp>
#include <string>
#include <system_error>

#include "httplib/jwt/error.hpp"

namespace httplib::jwt::detail
{
    std::string jwt_error_message(int ev) noexcept;
    boost::system::error_category const& jwt_boost_category() noexcept;
    std::error_category const& jwt_std_category() noexcept;
    std::error_code std_of(error e) noexcept;
} // namespace httplib::jwt::detail
