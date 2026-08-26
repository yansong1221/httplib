#pragma once
#include "lazy_response_impl.h"
#include "sse_event_parser.hpp"
#include <array>

namespace httplib::client
{
    class sse_reader_impl : public sse_reader
    {
      public:
        explicit sse_reader_impl(std::shared_ptr<lazy_response::impl> impl) : impl_(std::move(impl)) {}

        net::awaitable<boost::system::result<sse_event>>
        read_event() override
        {
            while (!parser_.has_event() && !impl_->is_body_done())
            {

                auto result = co_await impl_->read_some(net::buffer(read_buf_));
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
        is_done() const override
        {
            return impl_->is_body_done() && !parser_.has_event();
        }

      private:
        std::shared_ptr<lazy_response::impl> impl_;
        detail::sse_event_parser parser_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
