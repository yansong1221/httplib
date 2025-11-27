#pragma once
#include "httplib/body/any_body.hpp"
#include <any>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <regex>

namespace httplib {

struct request
{
public:
    request() = default;
    request(http::request<body::any_body>&& other);
    request(const http::request_header<>& other);

public:
    http::request_header<>& header() { return message_; }
    auto& body() { return message_.body(); }

    bool keep_alive() const { return message_.keep_alive(); }

    net::ip::address get_client_ip() const;

    void set_custom_data(std::any&& data);
    template<typename T>
    T custom_data() const
    {
        return std::any_cast<T>(custom_data_);
    }

public:
    std::string path;
    html::query_params query_params;
    std::unordered_map<std::string, std::string> path_params;
    std::smatch matches;
    tcp::endpoint local_endpoint;
    tcp::endpoint remote_endpoint;

private:
    std::any custom_data_;
    http::request<body::any_body> message_;
};


} // namespace httplib