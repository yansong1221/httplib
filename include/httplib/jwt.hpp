#pragma once
/// \brief JWT 模块公共入口（类型分布于 jwt/ 子目录）。
///
/// 使用方式与 jwt-cpp 文档同形：
/// \code
///     auto token = httplib::jwt::create()
///                      .set_subject("alice").set_expires_in(1h)
///                      .sign(httplib::jwt::hs256("secret"));
///
///     auto decoded = httplib::jwt::decode(token);
///     httplib::jwt::verify(httplib::jwt::hs256("secret"))
///         .with_issuer("my-app")
///         .verify(decoded);
/// \endcode
///
/// 实现侧 lib/jwt/ —— 各项语义工作（base64url / HMAC / 解析 / 组装 / 校验）
/// 全部委托 jwt-cpp（boost-json traits 实例化）；公共 API 只暴露 boost::json
/// 与本库类型，不泄漏 jwt-cpp 类型（与 Beast 同规）。

#include "httplib/jwt/error.hpp"
#include "httplib/jwt/algorithm.hpp"
#include "httplib/jwt/decoded_jwt.hpp"
#include "httplib/jwt/builder.hpp"
#include "httplib/jwt/verifier.hpp"
