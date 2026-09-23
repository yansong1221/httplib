#pragma once
#include "httplib/config.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/core/detail/clamp.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace httplib::body
{

    struct string_body
    {
      public:
        /** The type of container used for the body

            This determines the type of @ref message::body
            when this body type is used with a message container.
        */
        using value_type = std::string;

        /** The algorithm for parsing the body

            Meets the requirements of <em>BodyReader</em>.
        */
        class reader
        {
            value_type& body_;

          public:
            explicit reader(http::fields const&, value_type& b) : body_(b) {}

            void
            init(boost::optional<std::uint64_t> const& length, beast::error_code& ec)
            {
                if (length)
                {
                    if (*length > body_.max_size())
                    {
                        ec = http::error::buffer_overflow;
                        return;
                    }
                    body_.reserve(beast::detail::clamp(*length));
                }
                ec = {};
            }

            std::size_t
            put(net::const_buffer const& buffers, beast::error_code& ec)
            {
                auto const extra = net::buffer_size(buffers);
                auto const size = body_.size();
                if (extra > body_.max_size() - size)
                {
                    ec = http::error::buffer_overflow;
                    return 0;
                }

                body_.resize(size + extra);
                ec = {};
                char* dest = &body_[size];
                for (auto b : beast::buffers_range_ref(buffers))
                {
                    std::char_traits<char>::copy(dest, static_cast<char const*>(b.data()), b.size());
                    dest += b.size();
                }
                return extra;
            }

            void
            finish(beast::error_code& ec)
            {
                ec = {};
            }
        };

        /** The algorithm for serializing the body

            Meets the requirements of <em>BodyWriter</em>.
        */
        class writer
        {
            value_type const& body_;

          public:
            using const_buffers_type = net::const_buffer;

            explicit writer(http::fields const&, value_type const& b) : body_(b) {}

            void
            init(beast::error_code& ec)
            {
                ec = {};
            }

            boost::optional<std::pair<const_buffers_type, bool>>
            get(beast::error_code& ec)
            {
                ec = {};
                return {
                    { const_buffers_type { body_.data(), body_.size() }, false }
                };
            }
        };
    };

} // namespace httplib::body
