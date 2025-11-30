#pragma once
#include "httplib/body/any_body.hpp"
#include <any>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <regex>

namespace httplib::server {

struct request : public http::request<body::any_body>
{
public:
    request(tcp::endpoint local_endpoint,
            tcp::endpoint remote_endpoint,
            http::request<body::any_body>&& other);

    request(tcp::endpoint local_endpoint,
            tcp::endpoint remote_endpoint,
            http::request<http::empty_body>&& other);

    request& operator=(request&& other) noexcept;
    request(request&& other) noexcept;

    ~request();

    std::string decoded_path() const;
    const html::query_params& decoded_query_params() const;

    net::ip::address get_client_ip() const;
    const tcp::endpoint& local_endpoint() const;
    const tcp::endpoint& remote_endpoint() const;

    void set_custom_data(std::any&& data);
    std::any& custom_data();

public:
    class impl;
    std::unique_ptr<impl> impl_;

    std::unordered_map<std::string, std::string> path_params;
};


} // namespace httplib::server