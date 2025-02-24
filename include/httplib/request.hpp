#pragma once
#include "variant_message.hpp"

namespace httplib
{

struct request : public http::request<body::any_body>
{
    using http::request<body::any_body>::message;

    request(http::request<body::any_body>&& other) { http::request<body::any_body>::operator=(std::move(other)); }

public:
    std::unordered_map<std::string, std::string> params;
    std::smatch matches;
    net::ip::tcp::endpoint local_endpoint;
    net::ip::tcp::endpoint remote_endpoint;
};


} // namespace httplib