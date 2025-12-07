
#include "httplib/server/request.hpp"

namespace httplib::server {

request::request(tcp::endpoint local_endpoint,
                 tcp::endpoint remote_endpoint,
                 http::request<body::any_body>&& other)
    : http::request<body::any_body>(std::move(other))
    , local_endpoint_(std::move(local_endpoint))
    , remote_endpoint_(std::move(remote_endpoint))
{
    if (auto pos = this->target().find("?"); pos == std::string_view::npos) {
        this->decoded_path_ = util::url_decode(this->target());
    }
    else {
        this->decoded_path_ = util::url_decode(this->target().substr(0, pos));
        bool is_valid       = true;
        this->query_params_ =
            html::parse_http_query_params(this->target().substr(pos + 1), is_valid);
    }
}
request::request(tcp::endpoint local_endpoint,
                 tcp::endpoint remote_endpoint,
                 http::request<http::empty_body>&& other)
    : request(std::move(local_endpoint),
              std::move(remote_endpoint),
              http::request<body::any_body>(other))
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

std::string_view request::decoded_path() const
{
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

const httplib::html::query_params& request::decoded_query_params() const
{
    return this->query_params_;
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

std::vector<std::string_view> request::query_param(const std::string& key) const
{
    std::vector<std::string_view> values;
    auto range = query_params_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        values.push_back(it->second);
    }
    return values;
}

std::string_view request::query_param_front(const std::string& key) const
{
    auto iter = query_params_.find(key);
    if (iter == query_params_.end()) {
        throw std::runtime_error("Key not found: " + key);
    }
    return iter->second;
}

bool request::has_query_param(const std::string& key) const
{
    return query_params_.find(key) != query_params_.end();
}


} // namespace httplib::server