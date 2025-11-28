#include "request_impl.h"

namespace httplib {

request_impl::request_impl(tcp::endpoint local_endpoint,
                           tcp::endpoint remote_endpoint,
                           http::request<body::any_body>&& other)
    : local_endpoint_(std::move(local_endpoint))
    , remote_endpoint_(std::move(remote_endpoint))
    , message_(std::move(other))
{
    parse_target();
}

request_impl::request_impl(tcp::endpoint local_endpoint,
                           tcp::endpoint remote_endpoint,
                           const header_type& header)
    : local_endpoint_(std::move(local_endpoint))
    , remote_endpoint_(std::move(remote_endpoint))
    , message_(header)
{
    parse_target();
}

httplib::request::header_type& request_impl::header()
{
    return message_.base();
}

httplib::net::ip::address request_impl::get_client_ip() const
{
    auto iter = message_.find("X-Forwarded-For");
    if (iter == message_.end())
        return remote_endpoint_.address();

    auto tokens = util::split(iter->value(), ",");
    if (tokens.empty())
        return remote_endpoint_.address();

    boost::system::error_code ec;
    auto address = net::ip::make_address(tokens.front(), ec);
    if (ec)
        return remote_endpoint_.address();
    return address;
}

const tcp::endpoint& request_impl::local_endpoint() const
{
    return local_endpoint_;
}

const tcp::endpoint& request_impl::remote_endpoint() const
{
    return remote_endpoint_;
}

bool request_impl::keep_alive() const
{
    return message_.keep_alive();
}

void request_impl::set_custom_data(std::any&& data)
{
    custom_data_ = std::move(data);
}

std::any& request_impl::custom_data()
{
    return custom_data_;
}

const std::string& request_impl::as_string() const
{
    return message_.body().as<body::string_body>();
}

const boost::json::value& request_impl::as_json() const
{
    return message_.body().as<body::json_body>();
}

const httplib::html::query_params& request_impl::decoded_query_params() const
{
    return query_params_;
}

std::string_view request_impl::decoded_path() const
{
    return decoded_path_;
}

void request_impl::parse_target()
{
    auto tokens = util::split(message_.target(), "?");
    if (tokens.empty() || tokens.size() > 2)
        return;

    this->decoded_path_ = util::url_decode(tokens[0]);
    if (tokens.size() >= 2) {
        bool is_valid       = true;
        this->query_params_ = html::parse_http_query_params(tokens[1], is_valid);
        if (!is_valid)
            return;
    }
}

} // namespace httplib