#pragma once
#include "httplib/client/read_session.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include "streaming/sse_event_parser.hpp"
#include <array>

namespace httplib::client
{
    class sse_reader_impl : public httplib::client::sse_reader
    {
      public:
        explicit sse_reader_impl(std::shared_ptr<client::read_session> session) : session_(std::move(session)) {}

        net::awaitable<boost::system::result<sse_event>>
        read_event() override
        {
            while (!parser_.has_event() && !done_)
            {
                std::array<char, 4096> buf;
                auto result = co_await session_->read_body(net::buffer(buf));
                if (result.has_error())
                {
                    co_return result.error();
                }
                if (result.value() == 0)
                {
                    done_ = true;
                    break;
                }
                parser_.feed(std::string_view(buf.data(), result.value()));
            }
            if (parser_.has_event())
            {
                co_return parser_.next();
            }
            co_return sse_event {};
        }

        bool
        is_done() const override
        {
            return done_ && !parser_.has_event();
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

      private:
        std::shared_ptr<client::read_session> session_;
        streaming::detail::sse_event_parser parser_;
        bool done_ = false;
    };

} // namespace httplib::client
