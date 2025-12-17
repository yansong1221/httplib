#pragma once
#include "httplib/body/any_body.hpp"
#include <any>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>

namespace httplib::server {

struct request : public http::request<body::any_body>
{
public:
    request(const tcp::endpoint& local_endpoint,
            const tcp::endpoint& remote_endpoint,
            http::request<body::any_body>&& other);

    request(const tcp::endpoint& local_endpoint,
            const tcp::endpoint& remote_endpoint,
            http::request<http::empty_body>&& other);

    request& operator=(request&& other) noexcept;
    request(request&& other) noexcept;

    ~request();

public:
    std::string_view path() const;
    const html::query_params& query_params() const;

    net::ip::address get_client_ip() const;
    const tcp::endpoint& local_endpoint() const;
    const tcp::endpoint& remote_endpoint() const;

    void set_custom_data(std::any&& data);
    template<typename T>
    inline auto custom_data()
    {
        return std::any_cast<T>(custom_data_);
    }

    std::string_view path_param(const std::string& key) const;
    void add_path_param(const std::string& key, const std::string& val);
    void set_path_param(std::unordered_map<std::string, std::string>&& params);


private:
    std::string decoded_path_;
    html::query_params query_params_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;

    std::unordered_map<std::string, std::string> path_params_;
    std::any custom_data_;
};


} // namespace httplib::server