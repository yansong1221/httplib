#pragma once
#include "httplib/jwt.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>

namespace httplib::server::middleware
{

    class HTTPLIB_API jwt_auth_middleware
    {
      public:
        using value_type = jwt::decoded_jwt;

        /// \brief 请求级 JWT 鉴权中间件。
        ///
        /// verifier（算法白名单 + iss / aud 检查）在配置期构建完成，
        /// verify() 为 const 可跨并发请求复用；`with_*` 仅限 run() 之前调用
        ///（配置期，与 router 注册同约定）。
        explicit jwt_auth_middleware(jwt::algorithm const& alg);
        jwt_auth_middleware(jwt_auth_middleware const& other);
        jwt_auth_middleware& operator=(jwt_auth_middleware const& other);
        jwt_auth_middleware(jwt_auth_middleware&&) noexcept;
        jwt_auth_middleware& operator=(jwt_auth_middleware&&) noexcept;
        ~jwt_auth_middleware();

        /// \brief 校验 iss。
        jwt_auth_middleware& with_issuer(std::string_view iss);
        /// \brief 校验 aud。
        jwt_auth_middleware& with_audience(std::string_view aud);
        /// \brief Authorization 头的 scheme 前缀，默认 "Bearer"。
        jwt_auth_middleware& with_scheme(std::string_view scheme);
        /// \brief 改从其他头取 token（设置后不再使用 Authorization）。
        jwt_auth_middleware& with_header_name(std::string_view name);

        bool before(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
