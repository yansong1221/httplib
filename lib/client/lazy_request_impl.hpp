#pragma once

#include "body/body_writer.hpp"
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
        using body_writer_t = httplib::detail::body_writer<true, http_client::impl>;

        explicit lazy_request_impl(net::any_io_executor ex, std::shared_ptr<http_client::impl> parent)
            : executor_(std::move(ex))
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

            method_ = method;
            req_msg_ = std::make_unique<http::request<http::buffer_body>>(method, target, 11);
            req_msg_->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            for (auto const& f : headers)
            {
                req_msg_->set(f.name_string(), f.value());
            }
            req_msg_->set(http::field::host, parent_->host_value_);
            req_msg_->keep_alive(true);

            writer_ = std::make_unique<body_writer_t>(*req_msg_, parent_.get(), executor_);
            auto writer_mode
                = m == mode::chunked ? body_writer_t::stream_mode::chunked : body_writer_t::stream_mode::relay;
            ec = co_await writer_->begin_stream(writer_mode);
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
            if (!parent_ || !writer_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }
            ec = co_await writer_->write_some(data, more);
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

      private:
        net::any_io_executor executor_;
        std::shared_ptr<http_client::impl> parent_;
        http::verb method_ = http::verb::unknown;
        std::unique_ptr<http::request<http::buffer_body>> req_msg_;
        std::unique_ptr<body_writer_t> writer_;
    };
} // namespace httplib::client
