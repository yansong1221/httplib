#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/form_data.hpp"
#include "httplib/html.hpp"
#include "httplib/response.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/awaitable.hpp>

namespace httplib {

class response_impl : public httplib::response
{
public:
    response_impl(http_variant_stream_type& stream, unsigned int version, bool keep_alive);
    response_impl(http_variant_stream_type& stream, http::response<body::any_body>&& other);


    bool keep_alive() const override;
    header_type& header() override;


    void set_empty_content(http::status status) override;


    void set_error_content(http::status status) override;


    void set_string_content(std::string&& data,
                            std::string_view content_type,
                            http::status status = http::status::ok) override;


    void set_json_content(boost::json::value&& data,
                          http::status status = http::status::ok) override;


    void set_file_content(const fs::path& path, const http::fields& req_header) override;


    void set_form_data_content(const std::vector<form_data::field>& data) override;


    void set_redirect(std::string_view url,
                      http::status status = http::status::moved_permanently) override;

public:
    net::awaitable<boost::system::error_code>
    reply(const std::chrono::steady_clock::duration& timeout,
          const std::vector<std::string_view>& accept_encodings = {});

private:
    http_variant_stream_type& stream_;
    http::response<body::any_body> message_;
};
} // namespace httplib