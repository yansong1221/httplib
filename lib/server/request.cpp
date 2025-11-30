
#include "httplib/server/request.hpp"

namespace httplib::server {


class request::impl
{
public:
    std::string decoded_path_;
    html::query_params query_params_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;

    std::any custom_data_;
};

request::request(tcp::endpoint local_endpoint,
                 tcp::endpoint remote_endpoint,
                 http::request<body::any_body>&& other)
    : http::request<body::any_body>(std::move(other))
    , impl_(std::make_unique<request::impl>())
{
    impl_->local_endpoint_  = local_endpoint;
    impl_->remote_endpoint_ = remote_endpoint;

    auto tokens = util::split(this->target(), "?");
    if (tokens.empty() || tokens.size() > 2)
        return;

    impl_->decoded_path_ = util::url_decode(tokens[0]);
    if (tokens.size() >= 2) {
        bool is_valid        = true;
        impl_->query_params_ = html::parse_http_query_params(tokens[1], is_valid);
        if (!is_valid)
            return;
    }
}
request::request(tcp::endpoint local_endpoint,
                 tcp::endpoint remote_endpoint,
                 http::request<http::empty_body>&& other)
    : request(local_endpoint, remote_endpoint, http::request<body::any_body>(other))
{
}
request& request::operator=(request&& other) noexcept
{
    this->impl_ = std::move(other.impl_);
    http::request<body::any_body>::operator=(std::move(other));
    return *this;
}
request::request(request&& other) noexcept
{
    impl_ = std::move(other.impl_);
    http::request<body::any_body>::operator=(std::move(other));
}


request::~request()
{
}

std::string request::decoded_path() const
{
    return impl_->decoded_path_;
}

httplib::net::ip::address request::get_client_ip() const
{
    auto iter = this->find("X-Forwarded-For");
    if (iter == this->end())
        return impl_->remote_endpoint_.address();

    auto tokens = util::split(iter->value(), ",");
    if (tokens.empty())
        return impl_->remote_endpoint_.address();

    boost::system::error_code ec;
    auto address = net::ip::make_address(tokens.front(), ec);
    if (ec)
        return impl_->remote_endpoint_.address();
    return address;
}

const httplib::tcp::endpoint& request::local_endpoint() const
{
    return impl_->local_endpoint_;
}

const httplib::tcp::endpoint& request::remote_endpoint() const
{
    return impl_->remote_endpoint_;
}

void request::set_custom_data(std::any&& data)
{
    impl_->custom_data_ = std::move(data);
}

std::any& request::custom_data()
{
    return impl_->custom_data_;
}

const httplib::html::query_params& request::decoded_query_params() const
{
    return impl_->query_params_;
}


} // namespace httplib::server