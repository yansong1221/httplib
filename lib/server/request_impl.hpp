#pragma once
#include "body/any_body.hpp"
#include "body/lazy_body_reader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <algorithm>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>

namespace httplib::server
{

    class request::impl
        : public http::request<body::any_body>
        , public httplib::detail::lazy_body_reader<true, request::impl>
    {
        friend class httplib::detail::lazy_body_reader<true, request::impl>;

      public:
        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             http::request<body::any_body>&& other,
             bool is_ssl = false)
            : http::request<body::any_body>(std::move(other))
            , local_endpoint_(local_endpoint)
            , remote_endpoint_(remote_endpoint)
            , is_ssl_(is_ssl)
        {
            if (auto pos = this->target().find("?"); pos == std::string_view::npos)
            {
                this->decoded_path_ = util::url_decode(this->target());
            }
            else
            {
                this->decoded_path_ = util::url_decode(this->target().substr(0, pos));
                this->query_params_.decode(this->target().substr(pos + 1));
            }
        }

        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             http::request<http::empty_body>&& other,
             bool is_ssl = false)
            : impl(local_endpoint, remote_endpoint, http::request<body::any_body>(other), is_ssl)
        {
        }

        impl& operator=(impl&& other) noexcept = default;
        impl(impl&& other) noexcept = default;

        std::string_view
        path() const
        {
            if (this->decoded_path_.empty())
            {
                return std::string_view(this->target());
            }

            return this->decoded_path_;
        }
        html::query_params const&
        query_params() const
        {
            return query_params_;
        }

        net::ip::address
        get_client_ip() const
        {
            auto iter = this->find("X-Forwarded-For");
            if (iter == this->end())
            {
                return this->remote_endpoint_.address();
            }

            auto tokens = util::split(iter->value(), ",");
            if (tokens.empty())
            {
                return this->remote_endpoint_.address();
            }

            boost::system::error_code ec;
            auto address = net::ip::make_address(tokens.front(), ec);
            if (ec)
            {
                return this->remote_endpoint_.address();
            }
            return address;
        }
        tcp::endpoint const&
        local_endpoint() const
        {
            return this->local_endpoint_;
        }
        tcp::endpoint const&
        remote_endpoint() const
        {
            return this->remote_endpoint_;
        }
        bool
        is_ssl() const
        {
            return is_ssl_;
        }

        request_data&
        data()
        {
            return data_;
        }
        request_data const&
        data() const
        {
            return data_;
        }

        // ---- lazy body reader（对照 client::response::impl）----

        struct lazy_body_read_ctx
        {
            http_stream* stream = nullptr;
            beast::flat_buffer* buffer = nullptr;
            std::chrono::steady_clock::duration read_timeout { 30 };
        };

        // 构造 lazy 请求（body 未读）：header 已解析完毕，保留 header_parser 供后续读取。
        void
        setup_lazy_reading(http_stream& stream,
                           beast::flat_buffer& buffer,
                           std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
                           std::chrono::steady_clock::duration read_timeout,
                           std::uint64_t body_limit,
                           html::form_data::param form_data_params = html::form_data::param {})
        {
            lazy_ctx_ = std::make_unique<lazy_body_read_ctx>();
            lazy_ctx_->stream = &stream;
            lazy_ctx_->buffer = &buffer;
            lazy_ctx_->read_timeout = read_timeout;
            form_data_params_ = std::move(form_data_params);
            start(std::move(header_parser), body_limit, stream.get_executor());
        }

        bool
        is_lazy() const
        {
            return lazy_ctx_ != nullptr;
        }

        html::form_data::param const&
        form_data_params() const
        {
            return form_data_params_;
        }

        std::string_view
        operator[](http::field name) const
        {
            return this->base()[name];
        }
        std::string_view
        operator[](std::string_view name) const
        {
            return this->base()[name];
        }
        std::string_view
        at(http::field name) const
        {
            return this->base().at(name);
        }
        std::string_view
        at(std::string_view name) const
        {
            return this->base().at(name);
        }

        bool
        has(http::field name) const
        {
            return this->base().find(name) != this->base().end();
        }
        bool
        has(std::string_view name) const
        {
            return this->base().find(name) != this->base().end();
        }
        void
        set(http::field name, std::string_view value)
        {
            this->base().set(name, value);
        }
        void
        set(std::string_view name, std::string_view value)
        {
            this->base().set(name, value);
        }
        void
        erase(http::field name)
        {
            this->base().erase(name);
        }
        void
        erase(std::string_view name)
        {
            this->base().erase(name);
        }

        std::string_view
        path_param(std::string const& key) const
        {
            auto it = path_params_.find(key);
            if (it != path_params_.end())
            {
                return it->second;
            }
            return {};
        }
        void
        set_path_param(std::string const& key, std::string const& val)
        {
            path_params_[key] = val;
        }
        void
        set_path_param(std::unordered_map<std::string, std::string>&& params)
        {
            path_params_ = std::move(params);
        }

        static request
        make_request(tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     http::request<body::any_body>&& other,
                     bool is_ssl = false)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other), is_ssl);
            return request(std::move(_impl));
        }
        static request
        make_request(tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     http::request<http::empty_body>&& other,
                     bool is_ssl = false)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other), is_ssl);
            return request(std::move(_impl));
        }

      private:
        std::string decoded_path_;
        html::query_params query_params_;

        tcp::endpoint local_endpoint_;
        tcp::endpoint remote_endpoint_;
        bool is_ssl_ = false;

        std::unordered_map<std::string, std::string> path_params_;
        request_data data_;

        // ---- lazy body reader state ----
        std::unique_ptr<lazy_body_read_ctx> lazy_ctx_;
        html::form_data::param form_data_params_;

        // ---- httplib::detail::lazy_body_reader data source / materialization ----
        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            lazy_ctx_->stream->expires_after(lazy_ctx_->read_timeout);
            co_await http::async_read_some(*lazy_ctx_->stream, *lazy_ctx_->buffer, parser, util::net_awaitable[ec]);
            lazy_ctx_->stream->expires_never();
        }
        bool
        reader_is_materialized() const
        {
            // 非 lazy 请求的 body 已在 session 阶段物化到本请求。
            return lazy_ctx_ == nullptr;
        }
        void
        store_body(http::request<body::any_body>&& msg)
        {
            this->body() = std::move(msg.body());
        }
    };
} // namespace httplib::server
