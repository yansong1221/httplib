#pragma once
#include "client_impl.h"
#include "httplib/body/any_body.hpp"
#include "httplib/client/response.hpp"
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <memory>
#include <optional>
#include <variant>

namespace httplib::client
{
    class response::impl : public std::enable_shared_from_this<response::impl>
    {
      public:
        // eager：直接构造已完成读入的响应
        impl() = default;
        explicit impl(http::response<body::any_body>&& msg) : msg_(std::move(msg)) {}

        static response
        make(http::response<body::any_body>&& msg)
        {
            return response(std::make_shared<impl>(std::move(msg)));
        }

        http::status result() const
        {
            if (msg_)
            {
                return msg_->result();
            }
            if (header_parser_)
            {
                return header_parser_->get().result();
            }
            return status_;
        }

        unsigned
        result_int() const
        {
            return static_cast<unsigned>(result());
        }

        http::fields const&
        headers() const
        {
            if (msg_)
            {
                return msg_->base();
            }
            if (header_parser_)
            {
                return header_parser_->get().base();
            }
            return header_;
        }

        http::fields&
        headers()
        {
            if (msg_)
            {
                return msg_->base();
            }
            if (header_parser_)
            {
                return header_parser_->get().base();
            }
            return header_;
        }

        // ---- eager accessors ----

        std::string const&
        as_string() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<std::string>(msg_->body());
        }

        boost::json::value const&
        as_json() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<boost::json::value>(msg_->body());
        }

        html::form_data const&
        as_form_data() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<html::form_data>(msg_->body());
        }

        html::query_params const&
        as_query_params() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<html::query_params>(msg_->body());
        }

        // ---- lazy reader state ----

        // 构造 lazy 响应（body 未读）
        static response
        make_lazy(std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
                  std::shared_ptr<http_client::impl> parent)
        {
            auto impl = std::make_shared<response::impl>();
            impl->parent_ = std::move(parent);
            impl->header_parser_ = std::move(header_parser);
            impl->parent_->read_impl_ = impl;
            return response(std::move(impl));
        }

        bool
        is_body_done() const
        {
            if (msg_)
            {
                return true;
            }
            return resp_parser_ && resp_parser_->is_done();
        }

        net::awaitable<boost::system::result<std::size_t>>
        read_some(net::mutable_buffer const& buf)
        {
            if (msg_)
            {
                co_return 0;
            }
            if (!resp_parser_)
            {
                if (!header_parser_)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                }
                status_ = header_parser_->get().result();
                header_ = header_parser_->get().base();
                resp_parser_ = std::make_unique<http::response_parser<http::buffer_body>>(std::move(*header_parser_));
                header_parser_.reset();
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

        net::awaitable<boost::system::result<response>>
        read_body(http_client::impl::body_setup_fn const& body_setup)
        {
            if (msg_)
            {
                co_return response(shared_from_this());
            }
            if (!header_parser_)
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }
            status_ = header_parser_->get().result();
            header_ = header_parser_->get().base();

            http::response_parser<body::any_body> body_parser(std::move(*header_parser_));
            header_parser_.reset();

            if (body_setup)
            {
                body_setup(body_parser.get());
            }
            auto ec = co_await parent_->async_read(body_parser, false);
            if (ec)
            {
                co_return ec;
            }
            msg_ = body_parser.release();
            if (parent_)
            {
                parent_->read_impl_.reset();
            }
            parent_.reset();
            co_return response(shared_from_this());
        }

        std::shared_ptr<http_client::impl> parent_;

      private:
        http::status status_ { http::status::unknown };
        http::fields header_;
        std::optional<http::response<body::any_body>> msg_;
        std::unique_ptr<http::response_parser<http::empty_body>> header_parser_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
    };
} // namespace httplib::client
