#pragma once
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/sse_writer.hpp"
#include <string>

namespace httplib::server
{

    class sse_writer_impl : public server::sse_writer
    {
      public:
        explicit sse_writer_impl(server::chunk_writer* cw) : cw_(cw) {}

        net::awaitable<void>
        begin()
        {
            http::fields headers;
            headers.set(http::field::content_type, "text/event-stream");
            headers.set(http::field::cache_control, "no-cache");
            if (auto ec = co_await cw_->write_header(http::status::ok, headers, false); ec)
            {
                throw boost::system::system_error(ec);
            }
        }

        net::awaitable<void>
        send_event(std::string_view data, std::string_view event, std::string_view id, bool more) override
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
            if (auto ec = co_await cw_->write_body(net::buffer(msg), more); ec)
            {
                throw boost::system::system_error(ec);
            }
        }

        net::awaitable<void>
        send_retry(std::chrono::milliseconds ms, bool more) override
        {
            auto msg = "retry: " + std::to_string(ms.count()) + "\n\n";
            if (auto ec = co_await cw_->write_body(net::buffer(msg), more); ec)
            {
                throw boost::system::system_error(ec);
            }
        }

        net::awaitable<void>
        send_comment(std::string_view comment, bool more) override
        {
            auto msg = std::string(": ") + std::string(comment) + "\n\n";
            if (auto ec = co_await cw_->write_body(net::buffer(msg), more); ec)
            {
                throw boost::system::system_error(ec);
            }
        }

      private:
        server::chunk_writer* cw_;
    };

} // namespace httplib::server
