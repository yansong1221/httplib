#include "httplib/request.hpp"

namespace httplib {

request::request(http::request<body::any_body>&& other)
{
    http::request<body::any_body>::operator=(std::move(other));
}

net::ip::address
request::get_client_ip() const
{
    auto iter = this->find("X-Forwarded-For");
    if (iter == this->end()) return remote_endpoint.address();

    auto tokens = util::split(iter->value(), ",");
    if (tokens.empty()) return remote_endpoint.address();

    boost::system::error_code ec;
    auto address = net::ip::make_address(tokens.front(), ec);
    if (ec) return remote_endpoint.address();
    return address;
}

} // namespace httplib