#include "httplib/request.hpp"

namespace httplib {

request::request(http::request<body::any_body>&& other)
    : message_(std::move(other))
{
}

request::request(const http::request_header<>& other)
    : message_(other)
{
}

net::ip::address request::get_client_ip() const
{
    auto iter = message_.find("X-Forwarded-For");
    if (iter == message_.end())
        return remote_endpoint.address();

    auto tokens = util::split(iter->value(), ",");
    if (tokens.empty())
        return remote_endpoint.address();

    boost::system::error_code ec;
    auto address = net::ip::make_address(tokens.front(), ec);
    if (ec)
        return remote_endpoint.address();
    return address;
}

void request::set_custom_data(std::any&& data)
{
    custom_data_ = std::move(data);
}
} // namespace httplib