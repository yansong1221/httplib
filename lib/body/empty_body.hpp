#pragma once
#include "httplib/config.hpp"
#include <boost/beast/core/detail/config.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/optional.hpp>
#include <cstdint>

namespace httplib::body
{
    struct empty_body
    {
        struct value_type
        {
        };
        struct reader
        {
            explicit reader(http::fields const&, value_type&) {}

            void
            init(boost::optional<std::uint64_t> const&, beast::error_code& ec)
            {
                ec = {};
            }

            std::size_t
            put(net::const_buffer const&, beast::error_code& ec)
            {
                ec = http::error::unexpected_body;
                return 0;
            }

            void
            finish(beast::error_code& ec)
            {
                ec = {};
            }
        };
        struct writer
        {
            using const_buffers_type = net::const_buffer;
            explicit writer(http::fields const&, value_type const&) {}
            void
            init(beast::error_code& ec)
            {
                ec = {};
            }
            boost::optional<std::pair<const_buffers_type, bool>>
            get(beast::error_code& ec)
            {
                ec = {};
                return boost::none;
            }
        };
    };
} // namespace httplib::body
