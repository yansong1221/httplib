#pragma once

#include "client_impl.h"
#include "httplib/client/lazy_request.hpp"
#include "response_impl.h"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/version.hpp>
#include <limits>

namespace httplib::client
{
    class http_client::impl::lazy_request_impl final : public lazy_request
    {
      public:
        explicit lazy_request_impl(net::any_io_executor ex, std::shared_ptr<http_client::impl> parent)
            : write_mutex_(std::move(ex))
            , parent_(std::move(parent))
        {
        }
        net::awaitable<void>
        write_header(http::verb method, std::string_view target, http::fields const& headers, mode m) override
        {
            boost::system::error_code ec;
            co_await write_header(method, target, headers, m, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_header(http::verb method,
                     std::string_view target,
                     http::fields const& headers,
                     mode m,
                     boost::system::error_code& ec) override
        {
            if (!parent_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }

            auto write_lock = co_await write_mutex_.lock();
            if (!write_lock)
            {
                ec = net::error::make_error_code(net::error::operation_aborted);
                co_return;
            }

            method_ = method;
            req_msg_ = std::make_unique<http::request<http::buffer_body>>(method, target, 11);
            req_sr_ = std::make_unique<http::request_serializer<http::buffer_body>>(*req_msg_);
            req_msg_->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            for (auto const& f : headers)
            {
                req_msg_->set(f.name_string(), f.value());
            }
            req_msg_->set(http::field::host, parent_->host_value_);
            req_msg_->keep_alive(true);
            if (m == mode::chunked)
            {
                req_msg_->chunked(true);
            }
            co_await parent_->async_write(*req_sr_, true, ec);
        }

        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more) override
        {
            boost::system::error_code ec;
            co_await write_body(data, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }

        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec) override
        {
            if (!parent_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }
            auto write_lock = co_await write_mutex_.lock();
            if (!write_lock)
            {
                ec = net::error::make_error_code(net::error::operation_aborted);
                co_return;
            }

            if (!req_msg_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }

            auto& body = req_msg_->body();
            body.data = data.size() > 0 ? const_cast<void*>(data.data()) : nullptr;
            body.size = data.size();
            body.more = more;

            co_await parent_->async_write(*req_sr_, false, ec);
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
        }

        net::awaitable<boost::system::result<client::response>>
        read_response_lazy() override
        {
            if (!parent_)
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }
            co_return co_await parent_->read_response_lazy(method_);
        }

        net::awaitable<boost::system::result<client::response>>
        read_response() override
        {
            if (!parent_)
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }
            auto result = co_await read_response_lazy();
            if (result.has_error())
            {
                co_return result.error();
            }
            if (auto ec = co_await result->read_body(); ec)
            {
                co_return ec;
            }
            co_return result;
        }
        util::async_mutex write_mutex_;

        std::shared_ptr<http_client::impl> parent_;
        http::verb method_ = http::verb::unknown;
        std::unique_ptr<http::request<http::buffer_body>> req_msg_;
        std::unique_ptr<http::request_serializer<http::buffer_body>> req_sr_;
    };
} // namespace httplib::client
