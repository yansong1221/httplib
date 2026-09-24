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
            boost::system::error_code ec;

            for (;;)
            {
                auto lf = buf_.find('\n');
                if (lf == std::string::npos)
                {
                    if (impl_->reader().is_body_done())
                    {
                        co_return boost::json::value {};
                    }

                    auto bytes = co_await impl_->reader().read_some_decompressed(net::buffer(read_buf_), ec);
                    if (ec)
                    {
                        co_return ec;
                    }
                    if (bytes != 0)
                    {
                        buf_.append(read_buf_.data(), bytes);
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
            return impl_->reader().is_body_done() && buf_.empty();
        }

      private:
        std::shared_ptr<response::impl> impl_;
        std::string buf_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
