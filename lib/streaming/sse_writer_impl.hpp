#pragma once
#include "httplib/streaming/chunk_writer.hpp"
#include "httplib/streaming/sse_writer.hpp"
#include <string>

namespace httplib::streaming
{

    class sse_writer_impl : public httplib::sse_writer
    {
      public:
        explicit sse_writer_impl(httplib::chunk_writer& cw) : cw_(cw) {}

        net::awaitable<void>
        send_event(std::string_view data, std::string_view event, std::string_view id) override
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
            co_await cw_.write_chunk(std::move(msg));
        }

        net::awaitable<void>
        send_retry(std::chrono::milliseconds ms) override
        {
            auto msg = "retry: " + std::to_string(ms.count()) + "\n\n";
            co_await cw_.write_chunk(std::move(msg));
        }

        net::awaitable<void>
        send_comment(std::string_view comment) override
        {
            auto msg = std::string(": ") + std::string(comment) + "\n\n";
            co_await cw_.write_chunk(std::move(msg));
        }

        net::awaitable<void>
        close() override
        {
            co_await cw_.close();
        }

      private:
        httplib::chunk_writer& cw_;
    };

} // namespace httplib::streaming
