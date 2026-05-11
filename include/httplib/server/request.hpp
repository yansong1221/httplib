#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include <any>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>

namespace httplib::server {

class HTTPLIB_API request
{
public:
    class impl;

    request(std::unique_ptr<impl>&& _impl);

    request& operator=(request&& other) noexcept;
    request(request&& other) noexcept;

    ~request();

public:
    http::verb method() const;
    std::string_view method_string() const;
    std::string_view target() const;

    http::fields& base();
    const http::fields& base() const;

    std::string_view path() const;
    const html::query_params& query_params() const;

    net::ip::address get_client_ip() const;
    const tcp::endpoint& local_endpoint() const;
    const tcp::endpoint& remote_endpoint() const;

    void set_custom_data(std::any&& data);
    std::any& any_custom_data();
    const std::any& any_custom_data() const;

    template<typename T>
    inline auto custom_data()
    {
        return std::any_cast<T>(any_custom_data());
    }

    std::string_view path_param(const std::string& key) const;
    void add_path_param(const std::string& key, const std::string& val);
    void set_path_param(std::unordered_map<std::string, std::string>&& params);

    httplib::body::any_body::value_type& body();
    const httplib::body::any_body::value_type& body() const;


    impl* get_impl();
    const impl* get_impl() const;

private:
    std::unique_ptr<impl> impl_;
};


} // namespace httplib::server