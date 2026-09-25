#pragma once
#include "response_impl.h"
#include "sse_event_parser.hpp"
#include <array>

namespace httplib::client
{
    class sse_reader_impl : public sse_reader
    {
      public:
        explicit sse_reader_impl(std::shared_ptr<response::impl> impl) : impl_(std::move(impl)) {}

        net::awaitable<boost::system::result<sse_event>>
        read_event() override
        {
            boost::system::error_code ec;
            while (!parser_.has_event() && !impl_->is_body_done())
            {

                auto bytes = co_await impl_->read_some_decompressed(net::buffer(read_buf_), ec);
                if (ec)
                {
                    co_return ec;
                }
                if (bytes == 0)
                {
                    break;
                }
                parser_.feed(std::string_view(read_buf_.data(), bytes));
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
            return impl_->is_body_done() && !parser_.has_event();
        }

      private:
        std::shared_ptr<response::impl> impl_;
        detail::sse_event_parser parser_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
