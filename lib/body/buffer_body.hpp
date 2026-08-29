#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace httplib::body
{

    /** A body type backed by a caller-provided buffer.

        The caller re-initializes @ref value_type::data / @ref value_type::size
        before each read. Any decoded octets that do not fit into the caller's
        buffer are accumulated in @ref value_type::pending so no data is lost
        (used by streaming reads through @ref any_body).
    */
    struct buffer_body
    {
        /** The type of container used for the body. */
        struct value_type
        {
            /** Pointer to the caller's buffer, else `nullptr`. */
            void* data = nullptr;

            /** Number of free octets at @ref data. */
            std::size_t size = 0;

            /** Overflow: decoded octets that did not fit into @ref data. */
            std::string pending;
        };

        /** The algorithm for parsing the body

            Meets the requirements of <em>BodyReader</em>.
        */
        class reader
        {
            value_type& body_;

          public:
            explicit reader(http::fields const&, value_type& b);

            void init(boost::optional<std::uint64_t> const& length, beast::error_code& ec);
            std::size_t put(net::const_buffer const& buffers, beast::error_code& ec);

            void finish(beast::error_code& ec);
        };

        /** The algorithm for serializing the body

            Meets the requirements of <em>BodyWriter</em>.
        */
        class writer
        {
            value_type const& body_;
            bool done_ = false;

          public:
            using const_buffers_type = net::const_buffer;

            explicit writer(http::fields const&, value_type const& b);

            void
            init(beast::error_code& ec)
            {
                ec = {};
            }

            boost::optional<std::pair<const_buffers_type, bool>> get(beast::error_code& ec);
        };
    };

} // namespace httplib::body
