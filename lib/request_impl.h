#pragma once
#include "httplib/request.hpp"
#include "stream/http_stream.hpp"

namespace httplib {

class request_impl : public request
{
public:
    request_impl(tcp::endpoint local_endpoint,
                 tcp::endpoint remote_endpoint,
                 http::request<body::any_body>&& other);
    request_impl(tcp::endpoint local_endpoint,
                 tcp::endpoint remote_endpoint,
                 const header_type& header);


    header_type& header() override;
    std::string_view decoded_path() const override;
    const html::query_params& decoded_query_params() const override;

    net::ip::address get_client_ip() const override;
    const tcp::endpoint& local_endpoint() const override;
    const tcp::endpoint& remote_endpoint() const override;


    bool keep_alive() const override;

    void set_custom_data(std::any&& data) override;
    std::any& custom_data() override;


    const std::string& as_string() const override;
    const boost::json::value& as_json() const override;


private:
    void parse_target();

private:
    std::string decoded_path_;
    html::query_params query_params_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;

    std::any custom_data_;
    http::request<body::any_body> message_;
};
} // namespace httplib