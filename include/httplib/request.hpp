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
    using header_type  = http::request_header<http::fields>;
    virtual ~request() = default;

    virtual header_type& header()                                  = 0;
    virtual std::string_view decoded_path() const                  = 0;
    virtual const html::query_params& decoded_query_params() const = 0;

    virtual const std::string& as_string() const      = 0;
    virtual const boost::json::value& as_json() const = 0;

    virtual bool keep_alive() const = 0;

    virtual net::ip::address get_client_ip() const       = 0;
    virtual const tcp::endpoint& local_endpoint() const  = 0;
    virtual const tcp::endpoint& remote_endpoint() const = 0;

    virtual void set_custom_data(std::any&& data) = 0;
    virtual std::any& custom_data()               = 0;

public:
    std::unordered_map<std::string, std::string> path_params;
    std::smatch matches;
};


} // namespace httplib