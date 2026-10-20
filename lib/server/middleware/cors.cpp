#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <algorithm>
#include <fmt/format.h>
#include "beast_alias.hpp"

namespace httplib::server::middleware
{

    class cors_middleware::impl
    {
      public:
        std::vector<std::string> origins_ { "*" };
        std::vector<std::string> methods_ { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
        std::vector<std::string> headers_ { "Origin", "Content-Type", "Accept", "Authorization" };
        bool credentials_ = false;
        int max_age_ = 0;

        static std::string
        join(std::string_view sep, std::vector<std::string> const& parts)
        {
            std::string result;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i > 0)
                {
                    result += sep;
                }
                result += parts[i];
            }
            return result;
        }

        /// 解析应写入 Access-Control-Allow-Origin 的值。返回空 `acao` 表示不回这个头：
        ///
        /// - 请求无 Origin：没有 CORS 语境，保持既有行为——通配或显式单值按配置
        ///   原样回应（非浏览器客户端拿到确定的头），显式多值则不回。
        /// - 通配（白名单含 "*"）：无凭据时保持 "*"；凭据开启时改为回显请求 Origin，
        ///   因为 ACAO:"*" 与 Access-Control-Allow-Credentials:true 同屏违反 Fetch 规范。
        /// - 显式白名单：请求 Origin 精确命中（逐字比较）才回显该 Origin，配合
        ///   Vary: Origin 让缓存按 Origin 区分变体；未命中则完全不回。
        struct cors_origin
        {
            std::string acao;
            bool vary = false;
        };

        cors_origin
        resolve_origin(std::string_view req_origin) const
        {
            bool wildcard = std::find(origins_.begin(), origins_.end(), "*") != origins_.end();

            if (req_origin.empty())
            {
                if (wildcard)
                {
                    return cors_origin { "*", false };
                }
                if (origins_.size() == 1)
                {
                    return cors_origin { origins_.front(), false };
                }
                return {};
            }

            if (wildcard)
            {
                if (credentials_)
                {
                    return cors_origin { std::string(req_origin), true };
                }
                return cors_origin { "*", false };
            }
            if (std::find(origins_.begin(), origins_.end(), req_origin) != origins_.end())
            {
                return cors_origin { std::string(req_origin), true };
            }
            return {};
        }

        /// 在响应上追加 Vary: Origin：已有 Vary 值时按逗号 token 精确判重（大小写
        /// 不敏感）后追加，不覆写调用方自设的缓存指令。
        static void
        append_vary_origin(response& resp)
        {
            auto existing = resp[std::string_view("Vary")];
            if (existing.empty())
            {
                resp.set(std::string_view("Vary"), std::string_view("Origin"));
                return;
            }

            for (size_t pos = 0; pos < existing.size();)
            {
                auto end = existing.find(',', pos);
                auto last = end == std::string_view::npos ? existing.size() : end;
                auto token = existing.substr(pos, last - pos);
                while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                {
                    token.remove_prefix(1);
                }
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                {
                    token.remove_suffix(1);
                }
                if (beast::iequals(token, "Origin"))
                {
                    return;
                }
                if (end == std::string_view::npos)
                {
                    break;
                }
                pos = end + 1;
            }

            resp.set(std::string_view("Vary"), fmt::format("{}, Origin", existing));
        }

        void
        apply(request& req, response& resp) const
        {
            auto origin = resolve_origin(req[field::origin]);
            if (origin.acao.empty())
            {
                return;
            }
            resp.set(std::string_view("Access-Control-Allow-Origin"), origin.acao);
            if (origin.vary)
            {
                append_vary_origin(resp);
            }
            if (!methods_.empty())
            {
                resp.set(std::string_view("Access-Control-Allow-Methods"), join(std::string_view(", "), methods_));
            }
            if (!headers_.empty())
            {
                resp.set(std::string_view("Access-Control-Allow-Headers"), join(std::string_view(", "), headers_));
            }
            if (credentials_)
            {
                resp.set(std::string_view("Access-Control-Allow-Credentials"), std::string_view("true"));
            }
            if (max_age_ > 0)
            {
                resp.set(std::string_view("Access-Control-Max-Age"), std::to_string(max_age_));
            }
        }
    };

    cors_middleware::cors_middleware() : impl_(std::make_unique<impl>()) {}
    cors_middleware::~cors_middleware() = default;

    cors_middleware::cors_middleware(cors_middleware const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}

    cors_middleware&
    cors_middleware::operator=(cors_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }

    cors_middleware::cors_middleware(cors_middleware&&) noexcept = default;
    cors_middleware& cors_middleware::operator=(cors_middleware&&) noexcept = default;

    cors_middleware&
    cors_middleware::allow_origin(std::string origin)
    {
        impl_->origins_.clear();
        impl_->origins_.push_back(std::move(origin));
        return *this;
    }

    cors_middleware&
    cors_middleware::allow_origins(std::vector<std::string> origins)
    {
        impl_->origins_ = std::move(origins);
        return *this;
    }

    cors_middleware&
    cors_middleware::allow_methods(std::vector<std::string> methods)
    {
        impl_->methods_ = std::move(methods);
        return *this;
    }

    cors_middleware&
    cors_middleware::allow_headers(std::vector<std::string> headers)
    {
        impl_->headers_ = std::move(headers);
        return *this;
    }

    cors_middleware&
    cors_middleware::allow_credentials(bool allow)
    {
        impl_->credentials_ = allow;
        return *this;
    }

    cors_middleware&
    cors_middleware::max_age(int seconds)
    {
        impl_->max_age_ = seconds;
        return *this;
    }

    bool
    cors_middleware::before(request& req, response& resp)
    {
        impl_->apply(req, resp);
        // OPTIONS 一律短接 handler，以 204 响应（预检语义；CORS 头已由 apply 落好）。
        if (req.method() == method::options)
        {
            resp.set_empty_content(status::no_content);
            return false;
        }

        return true;
    }

    bool
    cors_middleware::after(request&, response& resp)
    {
        return true;
    }

} // namespace httplib::server::middleware
