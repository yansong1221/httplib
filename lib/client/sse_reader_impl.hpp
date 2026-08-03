#pragma once
#include "httplib/client/read_session.hpp"
#include "sse_event_parser.hpp"
#include <array>

namespace httplib::client
{
    class sse_reader_impl : public sse_reader
    {
      public:
        explicit sse_reader_impl(std::shared_ptr<client::read_session> session) : session_(std::move(session)) {}

        net::awaitable<boost::system::result<sse_event>>
        read_event() override
        {
            while (!parser_.has_event() && !session_->is_body_done())
            {

                auto result = co_await session_->read_body(net::buffer(read_buf_));
                if (result.has_error())
                {
                    co_return result.error();
                }
                if (result.value() == 0)
                {
                    break;
                }
                parser_.feed(std::string_view(read_buf_.data(), result.value()));
            }
            if (parser_.has_event())
            {
                co_return parser_.next();
            }
            co_return sse_event {};
        }

        bool
        is_header_done() const override
        {
            return session_->is_header_done();
        }

        net::awaitable<boost::system::error_code>
        read_header() override
        {
            co_return co_await session_->read_header();
        }

        http::fields const&
        headers() const override
        {
            return session_->headers();
        }

        http::status
        result() const override
        {
            return session_->result();
        }

        bool
        is_done() const override
        {
            return session_->is_body_done() && !parser_.has_event();
        }

      private:
        std::shared_ptr<client::read_session> session_;
        detail::sse_event_parser parser_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
