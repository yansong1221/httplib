#pragma once
#include "httplib/client/read_session.hpp"
#include <array>
#include <boost/json/parse.hpp>
#include <boost/system/result.hpp>
#include <string>

namespace httplib::client
{

    class ndjson_reader_impl : public httplib::client::ndjson_reader
    {
      public:
        explicit ndjson_reader_impl(std::shared_ptr<httplib::client::read_session> session)
            : session_(std::move(session))
        {
        }

        net::awaitable<boost::system::result<boost::json::value>>
        read() override
        {
            for (;;)
            {
                auto lf = buf_.find('\n');
                if (lf == std::string::npos)
                {
                    if (session_->is_body_done())
                    {
                        co_return boost::json::value {};
                    }

                    auto result = co_await session_->read_body(net::buffer(read_buf_));
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
            return is_header_done() && session_->is_body_done() && buf_.empty();
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

      private:
        std::shared_ptr<httplib::client::read_session> session_;
        std::string buf_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
