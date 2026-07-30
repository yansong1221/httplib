#include "httplib/server/middleware/auth.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"

namespace httplib::server::middleware {

namespace {

std::string base64_decode(std::string_view encoded)
{
    static constexpr std::string_view table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string decoded;
    int val = 0, bits = -8;
    for (unsigned char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r')
            break;
        auto pos = table.find(c);
        if (pos == std::string_view::npos)
            continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

} // namespace

// ---- basic_auth_middleware::impl ----

class basic_auth_middleware::impl
{
public:
    basic_auth_middleware::validator_t validator_;
    std::string realm_;
};

basic_auth_middleware::basic_auth_middleware(validator_t validator, std::string realm)
    : impl_(std::make_unique<impl>(std::move(validator), std::move(realm)))
{
}

basic_auth_middleware::~basic_auth_middleware() = default;

basic_auth_middleware::basic_auth_middleware(const basic_auth_middleware& other)
    : impl_(std::make_unique<impl>(other.impl_->validator_, other.impl_->realm_))
{
}

basic_auth_middleware& basic_auth_middleware::operator=(const basic_auth_middleware& other)
{
    if (this != &other)
        impl_ = std::make_unique<impl>(other.impl_->validator_, other.impl_->realm_);
    return *this;
}

basic_auth_middleware::basic_auth_middleware(basic_auth_middleware&&) noexcept            = default;
basic_auth_middleware& basic_auth_middleware::operator=(basic_auth_middleware&&) noexcept = default;

bool basic_auth_middleware::before(request& req, response& resp)
{
    auto auth = std::string(req.base()[http::field::authorization]);
    if (auth.starts_with("Basic ")) {
        auto creds    = base64_decode(std::string_view(auth).substr(6));
        auto colon    = creds.find(':');
        auto username = std::string_view(creds).substr(0, colon);
        auto password = colon != std::string::npos
                            ? std::string_view(creds).substr(colon + 1)
                            : std::string_view{};
        if (impl_->validator_ && impl_->validator_(username, password))
            return true;
    }

    resp.set(http::field::www_authenticate,
             std::string("Basic realm=\"") + impl_->realm_ + '"');
    resp.set_json_content({{"error", "unauthorized"}}, http::status::unauthorized);
    return false;
}

// ---- bearer_auth_middleware::impl ----

class bearer_auth_middleware::impl
{
public:
    bearer_auth_middleware::validator_t validator_;
    std::string realm_;
};

bearer_auth_middleware::bearer_auth_middleware(validator_t validator, std::string realm)
    : impl_(std::make_unique<impl>(std::move(validator), std::move(realm)))
{
}

bearer_auth_middleware::~bearer_auth_middleware() = default;

bearer_auth_middleware::bearer_auth_middleware(const bearer_auth_middleware& other)
    : impl_(std::make_unique<impl>(other.impl_->validator_, other.impl_->realm_))
{
}

bearer_auth_middleware& bearer_auth_middleware::operator=(const bearer_auth_middleware& other)
{
    if (this != &other)
        impl_ = std::make_unique<impl>(other.impl_->validator_, other.impl_->realm_);
    return *this;
}

bearer_auth_middleware::bearer_auth_middleware(bearer_auth_middleware&&) noexcept            = default;
bearer_auth_middleware& bearer_auth_middleware::operator=(bearer_auth_middleware&&) noexcept = default;

bool bearer_auth_middleware::before(request& req, response& resp)
{
    auto auth = std::string(req.base()[http::field::authorization]);
    if (auth.starts_with("Bearer ")) {
        auto token = std::string_view(auth).substr(7);
        if (impl_->validator_ && impl_->validator_(token))
            return true;
    }

    resp.set(http::field::www_authenticate,
             std::string("Bearer realm=\"") + impl_->realm_ + '"');
    resp.set_json_content({{"error", "unauthorized"}}, http::status::unauthorized);
    return false;
}

} // namespace httplib::server::middleware
