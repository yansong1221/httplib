#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/server/request_data.hpp"
#include "httplib/server/server_fwd.hpp"
#include <any>
#include <charconv>
#include <type_traits>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httplib::server
{

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

        request_data& data();
        request_data const& data() const;

        std::string_view path_param(std::string const& key) const;

        template <typename T, typename = std::enable_if_t<std::integral<T> || std::floating_point<T> || std::is_same_v<T, std::string>>>
        T
        path_param(std::string const& key) const
        {
            auto sv = static_cast<std::string_view>(path_param(key));
            if constexpr (std::same_as<T, std::string>)
            {
                return std::string(sv);
            }
            else
            {
                T val {};
                auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
                if (ec != std::errc())
                    throw std::runtime_error("path_param: cannot convert '" + std::string(sv) + "'");
                return val;
            }
        }

        httplib::body::any_body::value_type& body();
        httplib::body::any_body::value_type const& body() const;

        bool is_chunked() const;
        chunk_reader* get_chunk_reader();

      private:
        friend impl&
        get_impl(request& self)
        {
            return *self.impl_;
        }
        friend impl const&
        get_impl(request const& self)
        {
            return *self.impl_;
        }
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server