#pragma once
#include "httplib/body/any_body.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <regex>

namespace httplib {

struct request : public http::request<body::any_body>
{
    using http::request<body::any_body>::message;

    request(http::request<body::any_body>&& other);

public:
    net::ip::address get_client_ip() const;

public:
    std::string path;
    html::query_params query_params;
    std::unordered_map<std::string, std::string> path_params;
    std::smatch matches;
    tcp::endpoint local_endpoint;
    tcp::endpoint remote_endpoint;
};


} // namespace httplib