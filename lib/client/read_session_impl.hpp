#pragma once
#include "client_impl.h"
#include "httplib/client/read_session.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <limits>

namespace httplib::client
{
    class read_session_impl
        : public read_session
        , public std::enable_shared_from_this<read_session_impl>
    {
      public:
        explicit read_session_impl(std::shared_ptr<http_client::impl> parent) : parent_(std::move(parent)) {}

        net::awaitable<boost::system::error_code>
        read_header()
        {
            resp_parser_ = std::make_unique<http::response_parser<http::buffer_body>>();
            resp_parser_->body_limit((std::numeric_limits<std::uint64_t>::max)());
            resp_parser_->header_limit((std::numeric_limits<std::uint32_t>::max)());

            co_return co_await parent_->async_read(*resp_parser_, true);
        }

        http::status
        result() const
        {
            return resp_parser_->get().result();
        }

        http::fields const&
        headers() const
        {
            return resp_parser_->get();
        }

        bool
        is_header_done() const
        {
            return resp_parser_ && resp_parser_->is_header_done();
        }

        bool
        is_body_done() const override
        {
            return resp_parser_ && resp_parser_->is_done();
        }

        net::awaitable<boost::system::result<std::size_t>>
        read_body(net::mutable_buffer const& buf) override
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

      private:
        std::shared_ptr<http_client::impl> parent_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
    };
} // namespace httplib::client
