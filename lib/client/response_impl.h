#pragma once
#include "body/any_body.hpp"
#include "body/empty_body.hpp"
#include "body/lazy_body_reader.hpp"
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
    class response::impl
        : public std::enable_shared_from_this<response::impl>
        , public httplib::detail::lazy_body_reader<false, response::impl>
    {
        friend class httplib::detail::lazy_body_reader<false, response::impl>;
        using lazy_reader = httplib::detail::lazy_body_reader<false, response::impl>;

      public:
        impl(std::shared_ptr<http_client::impl> parent,
             std::unique_ptr<http::response_parser<http::empty_body>> header_parser)
            : parent_(std::move(parent))
        {
            header_ = header_parser->get().base();
            if (auto len = header_parser->content_length(); len)
            {
                content_length_ = *len;
            }

            start(std::move(header_parser), parent_->body_limit_.load(), parent_->get_executor());
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

        bool
        is_body_done() const
        {
            if (msg_)
            {
                return true;
            }
            return lazy_reader::is_body_done();
        }

        // 移动取出已物化的 body（不拷贝，取出后本响应不再持有该 body）。
        template <typename T>
        T
        take_body()
        {
            return std::move(std::get<T>(msg_->body()));
        }

      private:
        std::shared_ptr<http_client::impl> parent_;
        http::response_header<http::fields> header_;

        std::optional<std::uint64_t> content_length_;
        std::optional<http::response<body::any_body>> msg_;

        // ---- httplib::detail::lazy_body_reader data source / materialization ----
        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            co_await parent_->async_read_some(parser, ec);
        }
        bool
        reader_is_materialized() const
        {
            return msg_.has_value();
        }
        void
        store_body(http::response<body::any_body>&& msg)
        {
            msg_ = std::move(msg);
            std::unique_lock<std::recursive_mutex> lck(parent_->stream_mutex_);
            parent_->read_impl_.reset();
        }
    };
} // namespace httplib::client
