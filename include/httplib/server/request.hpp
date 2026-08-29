#pragma once
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
#include "httplib/server/request_data.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/misc.hpp"
#include <any>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/json/value.hpp>
#include <boost/system/result.hpp>
#include <charconv>
#include <cstddef>
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

        std::string const& as_string() const;
        boost::json::value const& as_json() const;
        html::form_data const& as_form_data() const;
        html::query_params const& as_query_params() const;

        bool is_empty() const;
        bool is_string() const;
        bool is_json() const;
        bool is_form_data() const;
        bool is_query_params() const;

        // ---- body 懒读取（lazy handler）----
        // 是否为懒读取请求：body 尚未读入，需通过 read_* 系列异步读取。
        bool is_lazy() const;

        // ---- 未读完 body（lazy）：异步读取 ----
        // 读取剩余 body 并按指定类型物化，成功后可继续用 as_* 同步取引用。
        net::awaitable<boost::system::result<std::string>> read_string();
        net::awaitable<boost::system::result<boost::json::value>> read_json();
        net::awaitable<boost::system::result<html::form_data>> read_form_data();
        net::awaitable<boost::system::result<html::query_params>> read_query_params();

        // 读取剩余 body 并物化到本请求（按 content-type 自动派发 body 类型）
        net::awaitable<boost::system::result<void>> read_body();

        // 低层流式读：返回未解压的（原始）body 字节
        net::awaitable<boost::system::result<std::size_t>> read_some_raw(net::mutable_buffer const& buffer);

        // 低层流式读：返回解压后的（content-encoding 已解码）body 字节
        net::awaitable<boost::system::result<std::size_t>> read_some_decompressed(net::mutable_buffer const& buffer);
        bool is_body_done() const;

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