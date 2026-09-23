#pragma once
#include "body/any_body.hpp"
#include "body/body_reader.hpp"
#include "body/body_state.hpp"
#include "body/empty_body.hpp"
#include "client_impl.h"
#include "httplib/client/response.hpp"
#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <cstring>
#include <memory>
#include <optional>
#include <variant>

namespace httplib::client
{
    class response::impl : public std::enable_shared_from_this<response::impl>
    {
      public:
        // 数据源用 response::impl 自身：它是 http_client 的 friend，可访问私有的
        // http_client::impl；body_reader 只要求 Source 提供 read_some(parser, ec)。
        using body_reader_t = httplib::detail::body_reader<false, response::impl>;
        using body_setup_fn = typename body_reader_t::body_setup_fn;
        using message_t = typename body_reader_t::message_t;

        impl(std::shared_ptr<http_client::impl> parent,
             std::unique_ptr<http::response_parser<http::empty_body>> header_parser)
            : parent_(std::move(parent))
        {
            header_ = header_parser->get().base();
            if (auto len = header_parser->content_length(); len)
            {
                content_length_ = *len;
            }

            body_reader_.start(
                this,
                std::move(header_parser),
                parent_->body_limit_.load(),
                parent_->get_executor(),
                [this]
                {
                    std::unique_lock<std::recursive_mutex> lck(parent_->stream_mutex_);
                    parent_->read_impl_.reset();
                });
        }

        ~impl()
        {
            if (parent_ && !is_body_done())
            {
                parent_->close();
            }
        }
        static response
        create(std::unique_ptr<http::response_parser<http::empty_body>> header_parser,
               std::shared_ptr<http_client::impl> parent)
        {
            auto impl = std::make_shared<response::impl>(std::move(parent), std::move(header_parser));
            {
                std::unique_lock<std::recursive_mutex> lck(impl->parent_->stream_mutex_);
                impl->parent_->read_impl_ = impl;
            }
            return response(std::move(impl));
        }

        http::response_header<http::fields> const&
        header() const
        {
            return header_;
        }
        http::response_header<http::fields>&
        header()
        {
            return header_;
        }

        std::optional<std::uint64_t>
        content_length() const
        {
            return content_length_;
        }

        // ---- body reader ----

        httplib::body::body_state&
        body_state()
        {
            return body_reader_.state();
        }

        httplib::body::body_state const&
        body_state() const
        {
            return body_reader_.state();
        }

        bool
        is_body_done() const
        {
            return body_reader_.is_body_done();
        }

        net::awaitable<void>
        read_body(body_setup_fn const& body_setup, boost::system::error_code& ec)
        {
            co_await body_reader_.read_body(body_setup, ec);
        }

        net::awaitable<std::size_t>
        read_some_raw(net::mutable_buffer const& buffer, boost::system::error_code& ec)
        {
            co_return co_await body_reader_.read_some_raw(buffer, ec);
        }

        net::awaitable<std::size_t>
        read_some_decompressed(net::mutable_buffer const& buffer, boost::system::error_code& ec)
        {
            co_return co_await body_reader_.read_some_decompressed(buffer, ec);
        }

        // body_reader 的数据源接口：转发到连接读取。
        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            co_await parent_->async_read_some(parser, ec);
        }

      private:
        std::shared_ptr<http_client::impl> parent_;
        http::response_header<http::fields> header_;

        std::optional<std::uint64_t> content_length_;
        body_reader_t body_reader_;
    };
} // namespace httplib::client
