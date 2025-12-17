
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"

namespace httplib::server {

request::request(const tcp::endpoint& local_endpoint,
                 const tcp::endpoint& remote_endpoint,
                 http::request<body::any_body>&& other)
    : http::request<body::any_body>(std::move(other))
    , local_endpoint_(local_endpoint)
    , remote_endpoint_(remote_endpoint)
{
    if (auto pos = this->target().find("?"); pos == std::string_view::npos) {
        this->decoded_path_ = util::url_decode(this->target());
    }
    else {
        this->decoded_path_ = util::url_decode(this->target().substr(0, pos));
        this->query_params_.decode(this->target().substr(pos + 1));
    }
}
request::request(const tcp::endpoint& local_endpoint,
                 const tcp::endpoint& remote_endpoint,
                 http::request<http::empty_body>&& other)
    : request(local_endpoint, remote_endpoint, http::request<body::any_body>(other))
{
}
request& request::operator=(request&& other) noexcept
{
    if (this == std::addressof(other))
        return *this;

    http::request<body::any_body>::operator=(std::move(other));
    decoded_path_    = std::move(other.decoded_path_);
    query_params_    = std::move(other.query_params_);
    local_endpoint_  = std::move(other.local_endpoint_);
    remote_endpoint_ = std::move(other.remote_endpoint_);
    path_params_     = std::move(other.path_params_);
    custom_data_     = std::move(other.custom_data_);
    return *this;
}
request::request(request&& other) noexcept
{
    request::operator=(std::move(other));
}

request::~request()
{
}

std::string_view request::path() const
{
    if (this->decoded_path_.empty())
        return this->target();

    return this->decoded_path_;
}

httplib::net::ip::address request::get_client_ip() const
{
    auto iter = this->find("X-Forwarded-For");
    if (iter == this->end())
        return this->remote_endpoint_.address();

    auto tokens = util::split(iter->value(), ",");
    if (tokens.empty())
        return this->remote_endpoint_.address();

    boost::system::error_code ec;
    auto address = net::ip::make_address(tokens.front(), ec);
    if (ec)
        return this->remote_endpoint_.address();
    return address;
}

const httplib::tcp::endpoint& request::local_endpoint() const
{
    return this->local_endpoint_;
}

const httplib::tcp::endpoint& request::remote_endpoint() const
{
    return this->remote_endpoint_;
}

void request::set_custom_data(std::any&& data)
{
    this->custom_data_ = std::move(data);
}

std::string_view request::path_param(const std::string& key) const
{
    return path_params_.at(key);
}

void request::add_path_param(const std::string& key, const std::string& val)
{
    path_params_[key] = val;
}
void request::set_path_param(std::unordered_map<std::string, std::string>&& params)
{
    path_params_ = std::move(params);
}

const html::query_params& request::query_params() const
{
    return query_params_;
}

} // namespace httplib::server