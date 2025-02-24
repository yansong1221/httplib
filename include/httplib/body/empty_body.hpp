#pragma once
#include "httplib/config.hpp"
#include <boost/beast/core/detail/config.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/optional.hpp>
#include <cstdint>


namespace httplib::body
{
/** An empty <em>Body</em>

This body is used to represent messages which do not have a
message body. If this body is used with a parser, and the
parser encounters octets corresponding to a message body,
the parser will fail with the error @ref http::unexpected_body.

The Content-Length of this body is always 0.
*/
struct empty_body
{
    /** The type of container used for the body

        This determines the type of @ref message::body
        when this body type is used with a message container.
    */
    struct value_type
    {
    };
    struct reader
    {
        template<bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields>&, value_type&)
        {
        }

        void init(boost::optional<std::uint64_t> const&, beast::error_code& ec) { ec = {}; }

        template<class ConstBufferSequence>
        std::size_t put(ConstBufferSequence const&, beast::error_code& ec)
        {
            ec = http::error::unexpected_body;
            return 0;
        }

        void finish(beast::error_code& ec) { ec = {}; }
    };
    struct writer
    {
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        explicit writer(http::header<isRequest, Fields>& h, value_type const&)
        {
            h.set(http::field::content_length, std::to_string(0));
        }

        void init(beast::error_code& ec) { ec = {}; }

        boost::optional<std::pair<const_buffers_type, bool>> get(beast::error_code& ec)
        {
            ec = {};
            return boost::none;
        }
    };
};
} // namespace httplib::body