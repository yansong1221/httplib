#pragma once
#include "httplib/server/request_data.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/misc.hpp"
#include <any>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <charconv>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace boost::json
{
    class value;
}

namespace httplib::html
{
    class form_data;
    class query_params;
} // namespace httplib::html

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

        std::string const& as_string() const;
        boost::json::value const& as_json() const;
        html::form_data const& as_form_data() const;
        html::query_params const& as_query_params() const;

        bool is_empty() const;
        bool is_string() const;
        bool is_json() const;
        bool is_form_data() const;
        bool is_query_params() const;

        bool is_chunked() const;
        chunk_reader* get_chunk_reader();

        template <typename T = std::string_view>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        T
        path_param(std::string const& key) const
        {
            auto sv = this->path_param_raw(key);
            if constexpr (std::same_as<T, std::string_view>)
            {
                return sv;
            }
            else if constexpr (std::same_as<T, std::string>)
            {
                return std::string(sv);
            }
            else
            {
                return util::from_chars_strict<T>(sv);
            }
        }

      private:
        std::string_view path_param_raw(std::string const& key) const;

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