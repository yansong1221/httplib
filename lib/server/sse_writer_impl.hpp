#pragma once
#include "httplib/server/sse_writer.hpp"
#include "httplib/server/stream_writer.hpp"
#include <memory>
#include <string>
#include <utility>
#include "beast_alias.hpp"

namespace httplib::server
{

    class sse_writer_impl : public server::sse_writer
    {
      public:
        // 共享持有 stream_writer：SSE writer 可以安全地比 response 活得更久；
        // 越界写由 stream_writer 的 weak 检查收口为 operation_aborted。
        explicit sse_writer_impl(std::shared_ptr<server::stream_writer> cw) : cw_(std::move(cw)) {}

        net::awaitable<void>
        begin() override
        {
            boost::system::error_code ec;
            co_await begin(ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        begin(boost::system::error_code& ec) override
        {
            headers headers;
            headers.set(field::content_type, "text/event-stream");
            headers.set(field::cache_control, "no-cache");
            co_await cw_->write_header(status::ok, headers, stream_writer::mode::chunked, ec);
        }
        net::awaitable<void>
        send_event(std::string_view data, std::string_view event, std::string_view id, bool more) override
        {
            boost::system::error_code ec;
            co_await send_event(data, event, id, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        send_event(std::string_view data,
                   std::string_view event,
                   std::string_view id,
                   bool more,
                   boost::system::error_code& ec) override
        {
            std::string msg;
            if (!id.empty())
            {
                msg += "id: ";
                msg += id;
                msg += "\n";
            }
            if (!event.empty())
            {
                msg += "event: ";
                msg += event;
                msg += "\n";
            }
            auto pos = data.find('\n');
            if (pos == std::string_view::npos)
            {
                msg += "data: ";
                msg += data;
                msg += "\n";
            }
            else
            {
                size_t start = 0;
                while (pos != std::string_view::npos)
                {
                    msg += "data: ";
                    auto line = data.substr(start, pos - start);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line = line.substr(0, line.size() - 1);
                    }
                    msg += line;
                    msg += "\n";
                    start = pos + 1;
                    pos = data.find('\n', start);
                }
                if (start < data.size())
                {
                    msg += "data: ";
                    auto line = data.substr(start);
                    if (!line.empty() && line.back() == '\r')
                    {
                        line = line.substr(0, line.size() - 1);
                    }
                    msg += line;
                    msg += "\n";
                }
            }
            msg += "\n";
            co_await cw_->write_body(net::buffer(msg), more, ec);
        }

        net::awaitable<void>
        send_retry(std::chrono::milliseconds ms, bool more) override
        {
            boost::system::error_code ec;
            co_await send_retry(ms, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        send_retry(std::chrono::milliseconds ms, bool more, boost::system::error_code& ec) override
        {
            auto msg = "retry: " + std::to_string(ms.count()) + "\n\n";
            co_await cw_->write_body(net::buffer(msg), more, ec);
        }
        net::awaitable<void>
        send_comment(std::string_view comment, bool more) override
        {
            boost::system::error_code ec;
            co_await send_comment(comment, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        send_comment(std::string_view comment, bool more, boost::system::error_code& ec) override
        {
            auto msg = std::string(": ") + std::string(comment) + "\n\n";
            co_await cw_->write_body(net::buffer(msg), more, ec);
        }

      private:
        std::shared_ptr<server::stream_writer> cw_;
    };

} // namespace httplib::server
