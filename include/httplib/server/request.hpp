#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include <any>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httplib::server
{

    namespace middleware
    {
        class session;
    }

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
        http::fields const& base() const;

        std::string_view operator[](http::field name) const;
        std::string_view operator[](std::string_view name) const;
        std::string_view at(http::field name) const;
        std::string_view at(std::string_view name) const;

        bool has(http::field name) const;
        bool has(std::string_view name) const;
        void set(http::field name, std::string_view value);
        void set(std::string_view name, std::string_view value);
        void erase(http::field name);
        void erase(std::string_view name);

        std::string_view path() const;
        html::query_params const& query_params() const;

        net::ip::address get_client_ip() const;
        tcp::endpoint const& local_endpoint() const;
        tcp::endpoint const& remote_endpoint() const;

        void set_custom_data(std::any&& data);
        std::any& custom_data();
        std::any const& custom_data() const;

        template <typename T>
        inline auto
        custom_data()
        {
            return std::any_cast<T>(custom_data());
        }

        void set_session(std::shared_ptr<middleware::session> sess);
        std::shared_ptr<middleware::session> session() const;

        std::string_view path_param(std::string const& key) const;
        void set_path_param(std::string const& key, std::string const& val);
        void set_path_param(std::unordered_map<std::string, std::string>&& params);

        httplib::body::any_body::value_type& body();
        httplib::body::any_body::value_type const& body() const;

        bool is_chunked() const;
        net::awaitable<boost::system::result<std::size_t>> read_chunk(net::mutable_buffer const& buffer);

        impl* get_impl();
        impl const* get_impl() const;

      private:
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server