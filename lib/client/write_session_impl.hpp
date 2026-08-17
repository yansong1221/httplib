#pragma once

#include "client_impl.h"
#include "httplib/client/write_session.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/serializer.hpp>

namespace httplib::client
{
    class http_client::impl::write_session_impl final : public write_session
    {
      public:
        explicit write_session_impl(std::shared_ptr<http_client::impl> parent) : parent_(std::move(parent)) {}

        net::awaitable<boost::system::error_code>
        write_header(http::verb method, std::string_view target, http::fields const& headers, bool relay) override
        {
            req_msg_ = std::make_unique<http::request<http::buffer_body>>(method, target, 11);
            req_sr_ = std::make_unique<http::request_serializer<http::buffer_body>>(*req_msg_);
            req_msg_->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            for (auto const& f : headers)
            {
                req_msg_->set(f.name_string(), f.value());
            }
            req_msg_->set(http::field::host, parent_->host_value_);
            req_msg_->keep_alive(true);
            if (!relay)
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

        std::shared_ptr<http_client::impl> parent_;
        std::unique_ptr<http::request<http::buffer_body>> req_msg_;
        std::unique_ptr<http::request_serializer<http::buffer_body>> req_sr_;
    };
} // namespace httplib::client
