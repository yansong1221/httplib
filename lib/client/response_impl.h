#pragma once
#include "body/any_body.hpp"
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
        impl(net::any_io_executor ex,
             std::shared_ptr<http_client::impl> parent,
             std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser)
            : parent_(std::move(parent))
            , read_mutex_(std::move(ex))
            , header_parser_(std::move(header_parser))
        {
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
            if (resp_parser_)
            {
                return resp_parser_->is_done();
            }
            if (dec_parser_)
            {
                if (!dec_parser_->is_done())
                {
                    return false;
                }
                // 解析器已读完，但解压溢出数据可能还没取完。
                if (auto const* buf_body = std::get_if<body::buffer_body::value_type>(&dec_parser_->get().body()))
                {
                    return buf_body->pending.empty();
                }
                return true;
            }
            return false;
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

            if (msg_)
            {
                ec = {};
                co_return 0;
            }

            if (!resp_parser_)
            {
                if (!header_parser_)
                {
                    ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                    co_return 0;
                }
                status_ = header_parser_->get().result();
                header_ = header_parser_->get().base();
                resp_parser_ = std::make_unique<http::response_parser<http::buffer_body>>(std::move(*header_parser_));
                header_parser_.reset();
            }

            for (;;)
            {
                // 已是 body 末尾：不要再发起底层读，直接结束。
                if (resp_parser_->is_done())
                {
                    ec = {};
                    co_return 0;
                }

                auto& body = resp_parser_->get().body();
                body.data = (void*)buf.data();
                body.size = buf.size();

                co_await parent_->async_read_some(*resp_parser_, ec);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }

                auto consumed = buf.size() - body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }

                if (ec)
                {
                    co_return 0;
                }
            }
        }

        // 流式读取解压后的 body：把 buffer_body 放进 any_body，复用其 content-encoding 解压逻辑。
        // 调用方缓冲写不下时溢出到 value_type::pending，下次调用先取 pending，保证不丢数据。
        net::awaitable<std::size_t>
        read_some_decompressed(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            auto read_lock = co_await read_mutex_.lock();
            if (!read_lock)
            {
                ec = net::error::make_error_code(net::error::operation_aborted);
                co_return 0;
            }

            if (msg_ || buf.size() == 0)
            {
                ec = {};
                co_return 0;
            }

            if (!dec_parser_)
            {
                if (!header_parser_)
                {
                    ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                    co_return 0;
                }
                status_ = header_parser_->get().result();
                header_ = header_parser_->get().base();

                dec_parser_ = std::make_unique<http::response_parser<body::any_body>>(std::move(*header_parser_));
                dec_parser_->get().body() = body::buffer_body::value_type {};
                dec_parser_->get().body().decompressed_limit = parent_->body_limit_.load();
                header_parser_.reset();
            }

            for (;;)
            {
                auto& buf_body = std::get<body::buffer_body::value_type>(dec_parser_->get().body());

                // 先取上一次没写完的溢出数据。
                if (!buf_body.pending.empty())
                {
                    auto n = std::min(buf.size(), buf_body.pending.size());
                    std::memcpy(buf.data(), buf_body.pending.data(), n);
                    buf_body.pending.erase(0, n);
                    ec = {};
                    co_return n;
                }

                // 已解压完且无溢出数据：不要再发起底层读，直接结束。
                if (dec_parser_->is_done())
                {
                    ec = {};
                    co_return 0;
                }

                buf_body.data = (void*)buf.data();
                buf_body.size = buf.size();

                co_await parent_->async_read_some(*dec_parser_, ec);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }

                auto consumed = buf.size() - buf_body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }
                if (ec)
                {
                    co_return 0;
                }
            }
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
            if (!header_parser_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }

            status_ = header_parser_->get().result();
            header_ = header_parser_->get().base();

            http::response_parser<body::any_body> body_parser(std::move(*header_parser_));
            body_parser.get().body().decompressed_limit = parent_->body_limit_.load();
            header_parser_.reset();

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

        http::status status_ { http::status::unknown };
        http::fields header_;
        std::optional<http::response<body::any_body>> msg_;
        std::unique_ptr<http::response_parser<http::empty_body>> header_parser_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
        // 解压流式读取专用：any_body 解析器，body 持有 buffer_body 值以走解压 reader。
        std::unique_ptr<http::response_parser<body::any_body>> dec_parser_;
    };
} // namespace httplib::client
