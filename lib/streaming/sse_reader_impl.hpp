#pragma once
#include "httplib/client/read_session.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include "streaming/sse_event_parser.hpp"
#include <array>

namespace httplib::streaming
{

    class sse_reader_impl : public httplib::sse_reader
    {
      public:
        explicit sse_reader_impl(std::shared_ptr<client::read_session> session)
            : session_(std::move(session))
        {
        }

        net::awaitable<sse_event>
        read_event() override
        {
            while (!parser_.has_event() && !done_)
            {
                std::array<char, 4096> buf;
                auto result = co_await session_->read_body(net::buffer(buf));
                if (result.has_error() || result.value() == 0)
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

      private:
        std::shared_ptr<client::read_session> session_;
        detail::sse_event_parser parser_;
        bool done_ = false;
    };

} // namespace httplib::streaming
