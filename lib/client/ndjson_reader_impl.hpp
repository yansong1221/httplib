#pragma once
#include "response_impl.h"
#include <array>
#include <boost/json/parse.hpp>
#include <boost/system/result.hpp>
#include <string>

namespace httplib::client
{

    class ndjson_reader_impl : public httplib::client::ndjson_reader
    {
      public:
        explicit ndjson_reader_impl(std::shared_ptr<response::impl> impl) : impl_(std::move(impl)) {}

        net::awaitable<boost::system::result<boost::json::value>>
        read() override
        {
            for (;;)
            {
                auto lf = buf_.find('\n');
                if (lf == std::string::npos)
                {
                    if (impl_->is_body_done())
                    {
                        co_return boost::json::value {};
                    }

                    auto result = co_await impl_->read_some(net::buffer(read_buf_));
                    if (result.has_error())
                    {
                        co_return result.error();
                    }
                    if (result.value() != 0)
                    {
                        buf_.append(read_buf_.data(), result.value());
                    }
                    continue;
                }

                auto line = buf_.substr(0, lf);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                buf_.erase(0, lf + 1);

                if (line.empty())
                {
                    continue;
                }

                co_return boost::json::parse(line);
            }
        }

        bool
        is_done() const override
        {
            return impl_->is_body_done() && buf_.empty();
        }

      private:
        std::shared_ptr<response::impl> impl_;
        std::string buf_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
