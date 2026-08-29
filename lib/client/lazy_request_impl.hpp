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
        explicit lazy_request_impl(std::shared_ptr<http_client::impl> parent) : parent_(std::move(parent)) {}

        net::awaitable<boost::system::error_code>
        write_header(http::verb method, std::string_view target, http::fields const& headers, bool relay) override
        {
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
            if (!relay && !req_msg_->has_content_length())
            {
                req_msg_->chunked(true);
            }

            co_return co_await parent_->async_write(*req_sr_, true);
        }

        net::awaitable<boost::system::error_code>
        write_body(net::const_buffer const& data, bool more) override
        {
            if (!req_msg_)
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }

            auto& body = req_msg_->body();
            body.data = (void*)data.data();
            body.size = data.size();
            body.more = more;

            auto ec = co_await parent_->async_write(*req_sr_, false, false);
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
            co_return ec;
        }

        net::awaitable<boost::system::result<client::response>>
        read_response_lazy() override
        {
            auto header_parser = std::make_unique<http::response_parser<http::empty_body>>();
            header_parser->skip(method_ == http::verb::head);
            header_parser->header_limit((std::numeric_limits<std::uint32_t>::max)());
            header_parser->body_limit((std::numeric_limits<std::uint64_t>::max)());

            if (auto ec = co_await parent_->async_read(*header_parser, true); ec)
            {
                co_return ec;
            }

            co_return client::response::impl::make_lazy(std::move(header_parser), parent_);
        }

        net::awaitable<boost::system::result<client::response>>
        read_response() override
        {
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

        std::shared_ptr<http_client::impl> parent_;
        http::verb method_ = http::verb::unknown;
        std::unique_ptr<http::request<http::buffer_body>> req_msg_;
        std::unique_ptr<http::request_serializer<http::buffer_body>> req_sr_;
    };
} // namespace httplib::client
