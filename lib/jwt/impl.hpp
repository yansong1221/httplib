#pragma once
/// \brief 三个 PIMPL 类的 impl 定义（仅实现侧可见；公共头只保留 class impl）。
///
/// 集中定义，供 decoded_jwt / builder / verifier 的实现文件互相取用
///（verifier 校验借道 decoded_jwt::impl::inner）。

#include "common.hpp"
#include <chrono>
#include <optional>
#include <string_view>

namespace httplib::jwt
{
    using detail::jtraits;
    using detail::jdecoded;
    using detail::jbuilder;
    using detail::jverifier;

    class decoded_jwt::impl
    {
      public:
        std::optional<jdecoded> inner;
        std::string alg;
        boost::system::error_code error_;

        explicit impl(std::string_view token_text)
        {
            try
            {
                // 直接构造引擎原生 string_type（boost::json::string，string_view
                // 一次拷入且自带 SSO），跳过 std::string 中转 —— token 可能是
                // 数百字节，这是每个请求一次的热路径。
                inner.emplace(jtraits::string_type { token_text });
                // 缺 alg / alg 非字符串按 invalid_token 记录（无法验签）。
                alg = inner->get_algorithm();
            }
            catch (std::bad_alloc const&)
            {
                throw;
            }
            catch (std::exception const&)
            {
                inner.reset();
                error_ = make_error_code(error::invalid_token);
            }
        }
    };

    class builder::impl
    {
      public:
        jbuilder engine;

        impl() : engine(std::chrono::system_clock {}) {}
    };

    class verifier::impl
    {
      public:
        jverifier engine;
        /// 白名单算法数（为 0 时 verify() 返回 not_initialized，fail-closed）。
        size_t allowed = 0;
        /// 翻译 token_expired 时区分 expired / not_yet_valid 的同款余量。
        std::chrono::seconds skew { 0 };

        impl() : engine(std::chrono::system_clock {}) {}
    };

} // namespace httplib::jwt
