#pragma once
#include "httplib/streaming/sse_reader.hpp"
#include <charconv>
#include <deque>
#include <string>
#include <string_view>

namespace httplib::streaming::detail
{

    class sse_event_parser
    {
      public:
        void
        feed(std::string_view data)
        {
            buf_.append(data);

            for (;;)
            {
                auto lf = buf_.find('\n');
                if (lf == std::string::npos)
                {
                    break;
                }

                auto line_len = lf;
                if (line_len > 0 && buf_[line_len - 1] == '\r')
                {
                    --line_len;
                }

                auto line = buf_.substr(0, line_len);
                buf_.erase(0, lf + 1);

                if (line_len == 0)
                {
                    if (has_any_field())
                    {
                        events_.push_back(emit());
                    }
                    continue;
                }

                if (line[0] == ':')
                {
                    continue;
                }

                auto colon = line.find(':');
                if (colon == std::string_view::npos)
                {
                    continue;
                }

                auto field = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                if (!value.empty() && value[0] == ' ')
                {
                    value = value.substr(1);
                }

                if (field == "id")
                {
                    id_ = value;
                }
                else if (field == "event")
                {
                    event_ = value;
                }
                else if (field == "data")
                {
                    if (!data_.empty())
                    {
                        data_ += '\n';
                    }
                    data_.append(value.data(), value.size());
                }
                else if (field == "retry")
                {
                    if (!value.empty())
                    {
                        unsigned long long n = 0;
                        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
                        if (ec == std::errc {} && ptr == value.data() + value.size())
                        {
                            retry_ = std::chrono::milliseconds(n);
                        }
                    }
                }
            }
        }

        bool
        has_event() const
        {
            return !events_.empty();
        }

        sse_event
        next()
        {
            auto ev = std::move(events_.front());
            events_.pop_front();
            return ev;
        }

      private:
        sse_event
        emit()
        {
            sse_event ev;
            ev.id = std::move(id_);
            ev.event = std::move(event_);
            ev.data = std::move(data_);
            ev.retry = retry_;

            id_ = {};
            event_ = {};
            data_ = {};
            retry_ = std::chrono::milliseconds { 0 };
            return ev;
        }

        bool
        has_any_field() const
        {
            return !id_.empty() || !event_.empty() || !data_.empty() || retry_ != std::chrono::milliseconds { 0 };
        }

        std::string buf_;
        std::deque<sse_event> events_;

        std::string id_;
        std::string event_;
        std::string data_;
        std::chrono::milliseconds retry_ { 0 };
    };

} // namespace httplib::streaming::detail
