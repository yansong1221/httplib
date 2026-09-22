#pragma once
#include "httplib/config.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace httplib
{
    /**
     * A self-contained WebSocket message: an owned payload plus its frame type.
     *
     * @details Carrying the text/binary flag together with the bytes removes the
     *          need to juggle separate `got_data()` / `got_binary()` accessors,
     *          and owning the payload decouples the message lifetime from the
     *          read buffer.
     */
    class websocket_message
    {
      public:
        websocket_message() = default;
        websocket_message(std::string data, bool binary) : data_(std::move(data)), binary_(binary) {}

        bool
        is_binary() const noexcept
        {
            return binary_;
        }
        bool
        is_text() const noexcept
        {
            return !binary_;
        }

        void
        set_binary(bool binary) noexcept
        {
            binary_ = binary;
        }

        std::string const&
        data() const& noexcept
        {
            return data_;
        }
        std::string&
        data() & noexcept
        {
            return data_;
        }
        std::string&&
        data() && noexcept
        {
            return std::move(data_);
        }

        std::string_view
        view() const noexcept
        {
            return data_;
        }
        std::size_t
        size() const noexcept
        {
            return data_.size();
        }
        bool
        empty() const noexcept
        {
            return data_.empty();
        }

      private:
        std::string data_;
        bool binary_ = false;
    };
} // namespace httplib
