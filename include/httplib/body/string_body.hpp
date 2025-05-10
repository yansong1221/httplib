#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace httplib::body {

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
        explicit reader(const http::fields&, value_type& b);

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

    public:
        using const_buffers_type = net::const_buffer;

        explicit writer(const http::fields&, value_type const& b);

        void init(beast::error_code& ec) { ec = {}; }

        boost::optional<std::pair<const_buffers_type, bool>> get(beast::error_code& ec);
    };
};

} // namespace httplib::body