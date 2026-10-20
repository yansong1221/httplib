#include "httplib/server/middleware/jwt_auth.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <spdlog/spdlog.h>
#include <string>
#include "beast_alias.hpp"

namespace httplib::server::middleware
{

    /// \brief 配置期状态：jwt::verifier 在构造期随算法一次性配好。
    ///
    /// jwt-cpp 的 verifier::verify() 是 const 且全程只读（无共享可变状态），
    /// 因此同一个 verifier 可以跨并发请求复用；每个请求只剩一次 token 解析拷贝。
    struct jwt_auth_middleware::impl
    {
        jwt::verifier verifier;
        std::string scheme = "Bearer";
        std::string header_name;
    };

    jwt_auth_middleware::jwt_auth_middleware(jwt::algorithm const& alg) : impl_(std::make_unique<impl>())
    {
        impl_->verifier.allow_algorithm(alg);
    }
    jwt_auth_middleware::jwt_auth_middleware(jwt_auth_middleware const& other)
        : impl_(std::make_unique<impl>(*other.impl_))
    {
    }
    jwt_auth_middleware::jwt_auth_middleware(jwt_auth_middleware&&) noexcept = default;
    jwt_auth_middleware::~jwt_auth_middleware() = default;
    jwt_auth_middleware&
    jwt_auth_middleware::operator=(jwt_auth_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    jwt_auth_middleware& jwt_auth_middleware::operator=(jwt_auth_middleware&&) noexcept = default;

    jwt_auth_middleware&
    jwt_auth_middleware::with_issuer(std::string_view iss)
    {
        impl_->verifier.with_issuer(iss);
        return *this;
    }

    jwt_auth_middleware&
    jwt_auth_middleware::with_audience(std::string_view aud)
    {
        impl_->verifier.with_audience(aud);
        return *this;
    }

    jwt_auth_middleware&
    jwt_auth_middleware::with_scheme(std::string_view scheme)
    {
        impl_->scheme = scheme;
        return *this;
    }

    jwt_auth_middleware&
    jwt_auth_middleware::with_header_name(std::string_view name)
    {
        impl_->header_name = name;
        return *this;
    }

    bool
    jwt_auth_middleware::before(request& req, response& resp)
    {
        auto& p = *impl_;
        std::string_view token;

        if (!p.header_name.empty())
        {
            if (token = req[p.header_name]; token.empty())
            {
                resp.set_error_content(status::unauthorized);
                return false;
            }
        }
        else
        {
            auto auth = req[field::authorization];
            auto prefix = p.scheme + " ";
            if (!auth.starts_with(prefix))
            {
                resp.set_error_content(status::unauthorized);
                return false;
            }
            token = auth.substr(prefix.size());
        }

        auto result = jwt::decode(token);
        if (result.has_error())
        {
            resp.set_error_content(status::unauthorized);
            return false;
        }

        boost::system::error_code ec;
        p.verifier.verify(result.value(), ec);
        if (ec)
        {
            resp.set_error_content(status::unauthorized);
            return false;
        }

        req.data().store(std::move(result.value()));
        return true;
    }

} // namespace httplib::server::middleware
