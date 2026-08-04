#pragma once
#include "chunked_body_reader.hpp"
#include "httplib/server/chunk_reader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <chrono>

namespace httplib::server
{

    class request::impl : public http::request<body::any_body>
    {
      public:
        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             http::request<body::any_body>&& other)
            : http::request<body::any_body>(std::move(other))
            , local_endpoint_(local_endpoint)
            , remote_endpoint_(remote_endpoint)
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
             http::request<http::empty_body>&& other)
            : impl(local_endpoint, remote_endpoint, http::request<body::any_body>(other))
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

        void
        set_custom_data(std::string key, std::any value)
        {
            custom_data_[std::move(key)] = std::move(value);
        }
        std::any&
        custom_data(std::string const& key)
        {
            return custom_data_.at(key);
        }
        std::any const&
        custom_data(std::string const& key) const
        {
            return custom_data_.at(key);
        }
        void
        erase_custom_data(std::string const& key)
        {
            custom_data_.erase(key);
        }
        bool
        has_custom_data(std::string const& key) const
        {
            return custom_data_.contains(key);
        }

        struct buffer_body_read_ctx
        {
            std::shared_ptr<http::request_parser<http::buffer_body>> parser;
            std::shared_ptr<chunk_reader> reader;
        };

        void
        setup_chunked_reading(http_stream& stream,
                              beast::flat_buffer& buffer,
                              http::request_parser<http::empty_body>&& header_parser,
                              std::chrono::steady_clock::duration read_timeout)
        {
            auto parser = std::make_shared<http::request_parser<http::buffer_body>>(std::move(header_parser));

            buffer_body_ctx_ = std::make_shared<buffer_body_read_ctx>();
            buffer_body_ctx_->parser = parser;

            auto reader = std::make_shared<chunked_body_reader>();
            reader->setup(stream, buffer, *parser, read_timeout);
            buffer_body_ctx_->reader = std::move(reader);
        }

        bool
        is_chunked() const
        {
            return buffer_body_ctx_ != nullptr;
        }

        chunk_reader*
        get_chunk_reader()
        {
            if (!buffer_body_ctx_)
            {
                return nullptr;
            }
            return buffer_body_ctx_->reader.get();
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
                     http::request<body::any_body>&& other)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
            return request(std::move(_impl));
        }
        static request
        make_request(tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     http::request<http::empty_body>&& other)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
            return request(std::move(_impl));
        }

      private:
        std::string decoded_path_;
        html::query_params query_params_;

        tcp::endpoint local_endpoint_;
        tcp::endpoint remote_endpoint_;

        std::unordered_map<std::string, std::string> path_params_;
        std::unordered_map<std::string, std::any> custom_data_;

        std::shared_ptr<buffer_body_read_ctx> buffer_body_ctx_;
    };
} // namespace httplib::server