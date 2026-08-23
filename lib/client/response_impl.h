#pragma once
#include "client_impl.h"
#include <array>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <limits>
#include <string>

namespace httplib::client
{
    class response::impl : public std::enable_shared_from_this<response::impl>
    {
      public:
        explicit impl(std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
                      std::shared_ptr<http_client::impl> parent)
            : header_parser_(std::move(header_parser))
            , parent_(std::move(parent))
        {
            status_ = header_parser_->get().result();
            header_ = header_parser_->get().base();
        }

        static net::awaitable<boost::system::result<client::response>>
        create(std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
               std::shared_ptr<http_client::impl> parent)
        {
            auto impl = std::make_shared<response::impl>(std::move(header_parser), std::move(parent));
            impl->parent_->read_impl_ = impl;
            co_return client::response(std::move(impl));
        }

        http::status
        result() const
        {
            return status_;
        }

        http::fields const&
        headers() const
        {
            return header_;
        }

        http::fields&
        headers()
        {
            return header_;
        }

        bool
        is_body_done() const
        {
            return resp_parser_ && resp_parser_->is_done();
        }

        net::awaitable<boost::system::result<std::size_t>>
        read_some(net::mutable_buffer const& buf)
        {
            if (!resp_parser_ || !resp_parser_->is_header_done())
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }

            for (;;)
            {
                auto& body = resp_parser_->get().body();
                body.data = (void*)buf.data();
                body.size = buf.size();

                auto ec = co_await parent_->async_read(*resp_parser_, false);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }
                if (ec)
                {
                    co_return ec;
                }

                auto consumed = buf.size() - body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }

                if (resp_parser_->is_done())
                {
                    co_return 0;
                }
            }
        }

        net::awaitable<boost::system::result<http_client::response>>
        read_body(http_client::impl::body_setup_fn const& body_setup)
        {
            http::response_parser<body::any_body> body_parser(std::move(header_parser_->get()));
            header_parser_.reset();

            if (body_setup)
            {
                if (!body_parser.is_done())
                {
                    body_setup(body_parser.get());
                }
            }
            auto ec = co_await parent_->async_read(body_parser, false);
            if (ec)
            {
                co_return ec;
            }
            co_return body_parser.release();
        }

      private:
        http::status status_ { http::status::unknown };
        http::fields header_;
        std::shared_ptr<http_client::impl> parent_;
        std::unique_ptr<http::response_parser<http::empty_body>> header_parser_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
    };
} // namespace httplib::client
