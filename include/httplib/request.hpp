#pragma once
#include "httplib/body/any_body.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <regex>

namespace httplib
{

struct request : public http::request<body::any_body>
{
    using http::request<body::any_body>::message;

    request(http::request<body::any_body>&& other) { http::request<body::any_body>::operator=(std::move(other)); }

public:
    std::unordered_map<std::string, std::string> params;
    std::smatch matches;
    tcp::endpoint local_endpoint;
    tcp::endpoint remote_endpoint;
};


} // namespace httplib