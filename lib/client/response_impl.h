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
        using base = httplib::detail::lazy_body_reader<false, response::impl>;

      public:
        impl(net::any_io_executor ex,
             std::shared_ptr<http_client::impl> parent,
             std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser)
            : parent_(std::move(parent))
            , read_mutex_(std::move(ex))
        {
            start(std::move(header_parser), parent_->body_limit_.load());
        }

        // eager：直接构造已完成读入的响应
        explicit impl(net::any_io_executor ex, http::response<body::any_body>&& msg)
            : read_mutex_(std::move(ex))
            , msg_(std::move(msg))
        {
        }
        ~impl()
        {
            if (parent_ && !is_body_done())
            {
                parent_->close();
            }
        }

        static response
        make(net::any_io_executor ex, http::response<body::any_body>&& msg)
        {
            return response(std::make_shared<impl>(std::move(ex), std::move(msg)));
        }

        http::status
        result() const
        {
            if (msg_)
            {
                return msg_->result();
            }
            return base::result();
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
            return base::headers();
        }

        http::fields&
        headers()
        {
            if (msg_)
            {
                return msg_->base();
            }
            return base::headers();
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
        make_lazy(net::any_io_executor ex,
                  std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
                  std::shared_ptr<http_client::impl> parent)
        {
            auto impl = std::make_shared<response::impl>(std::move(ex), std::move(parent), std::move(header_parser));
            {
                std::unique_lock<std::recursive_mutex> lck(impl->parent_->stream_mutex_);
                impl->parent_->read_impl_ = impl;
            }
            return response(std::move(impl));
        }

        bool
        is_body_done() const
        {
            if (msg_)
            {
                return true;
            }
            return base::is_body_done();
        }

        net::awaitable<std::size_t>
        read_some_raw(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            auto read_lock = co_await read_mutex_.lock();
            if (!read_lock)
            {
                ec = net::error::make_error_code(net::error::operation_aborted);
                co_return 0;
            }
            co_return co_await read_some_raw_impl(buf, ec);
        }

        net::awaitable<std::size_t>
        read_some_decompressed(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            auto read_lock = co_await read_mutex_.lock();
            if (!read_lock)
            {
                ec = net::error::make_error_code(net::error::operation_aborted);
                co_return 0;
            }
            co_return co_await read_some_decompressed_impl(buf, ec);
        }

        net::awaitable<void>
        read_body(http_client::impl::body_setup_fn const& body_setup, boost::system::error_code& ec)
        {
            auto read_lock = co_await read_mutex_.lock();
            if (!read_lock)
            {
                ec = net::error::make_error_code(net::error::operation_aborted);
                co_return;
            }

            if (msg_)
            {
                ec = {};
                co_return;
            }
            auto header_parser = take_header_parser();
            if (!header_parser)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }

            http::response_parser<body::any_body> body_parser(std::move(*header_parser));
            body_parser.get().body().decompressed_limit = parent_->body_limit_.load();

            if (body_setup)
            {
                body_setup(body_parser.get());
            }
            co_await parent_->async_read(body_parser, false, ec);
            if (ec)
            {
                co_return;
            }
            msg_ = body_parser.release();
            {
                std::unique_lock<std::recursive_mutex> lck(parent_->stream_mutex_);
                parent_->read_impl_.reset();
            }
        }

        // 移动取出已物化的 body（不拷贝，取出后本响应不再持有该 body）。
        template <typename T>
        T
        take_body()
        {
            return std::move(std::get<T>(msg_->body()));
        }

        std::shared_ptr<http_client::impl> parent_;

      private:
        util::async_mutex read_mutex_;

        std::optional<http::response<body::any_body>> msg_;

        // ---- httplib::detail::lazy_body_reader data source ----
        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            co_await parent_->async_read_some(parser, ec);
        }
    };
} // namespace httplib::client
