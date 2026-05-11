#pragma once
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include <boost/asio/ip/tcp.hpp>

namespace httplib::server {

class request::impl : public http::request<body::any_body>
{
public:
    impl(const tcp::endpoint& local_endpoint,
         const tcp::endpoint& remote_endpoint,
         http::request<body::any_body>&& other)
        : http::request<body::any_body>(std::move(other))
        , local_endpoint_(local_endpoint)
        , remote_endpoint_(remote_endpoint)
    {
        if (auto pos = this->target().find("?"); pos == std::string_view::npos) {
            this->decoded_path_ = util::url_decode(this->target());
        }
        else {
            this->decoded_path_ = util::url_decode(this->target().substr(0, pos));
            this->query_params_.decode(this->target().substr(pos + 1));
        }
    }

    impl(const tcp::endpoint& local_endpoint,
         const tcp::endpoint& remote_endpoint,
         http::request<http::empty_body>&& other)
        : impl(local_endpoint, remote_endpoint, http::request<body::any_body>(other))
    {
    }

    impl& operator=(impl&& other) noexcept
    {
        if (this == std::addressof(other))
            return *this;

        http::request<body::any_body>::operator=(std::move(other));
        decoded_path_    = std::move(other.decoded_path_);
        query_params_    = std::move(other.query_params_);
        local_endpoint_  = std::move(other.local_endpoint_);
        remote_endpoint_ = std::move(other.remote_endpoint_);
        path_params_     = std::move(other.path_params_);
        custom_data_     = std::move(other.custom_data_);
        return *this;
    }
    impl(impl&& other) noexcept { impl::operator=(std::move(other)); }

    std::string_view path() const
    {
        if (this->decoded_path_.empty())
            return std::string_view(this->target());

        return this->decoded_path_;
    }
    const html::query_params& query_params() const { return query_params_; }

    net::ip::address get_client_ip() const
    {
        auto iter = this->find("X-Forwarded-For");
        if (iter == this->end())
            return this->remote_endpoint_.address();

        auto tokens = util::split(iter->value(), ",");
        if (tokens.empty())
            return this->remote_endpoint_.address();

        boost::system::error_code ec;
        auto address = net::ip::make_address(tokens.front(), ec);
        if (ec)
            return this->remote_endpoint_.address();
        return address;
    }
    const tcp::endpoint& local_endpoint() const { return this->local_endpoint_; }
    const tcp::endpoint& remote_endpoint() const { return this->remote_endpoint_; }

    void set_custom_data(std::any&& data) { this->custom_data_ = std::move(data); }
    std::any& custom_data() { return custom_data_; }
    const std::any& custom_data() const { return custom_data_; }

    std::string_view path_param(const std::string& key) const { return path_params_.at(key); }
    void add_path_param(const std::string& key, const std::string& val) { path_params_[key] = val; }
    void set_path_param(std::unordered_map<std::string, std::string>&& params)
    {
        path_params_ = std::move(params);
    }

    static request make_request(const tcp::endpoint& local_endpoint,
                                const tcp::endpoint& remote_endpoint,
                                http::request<body::any_body>&& other)
    {
        auto _impl =
            std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
        return request(std::move(_impl));
    }
    static request make_request(const tcp::endpoint& local_endpoint,
                                const tcp::endpoint& remote_endpoint,
                                http::request<http::empty_body>&& other)
    {
        auto _impl =
            std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
        return request(std::move(_impl));
    }

private:
    std::string decoded_path_;
    html::query_params query_params_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;

    std::unordered_map<std::string, std::string> path_params_;
    std::any custom_data_;
};
} // namespace httplib::server