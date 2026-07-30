//
// Copyright (c) 2022 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: JSON body
//
//------------------------------------------------------------------------------

#ifndef BOOST_BEAST_EXAMPLE_JSON_BODY
#define BOOST_BEAST_EXAMPLE_JSON_BODY

#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/json/value.hpp>
#include <memory>

namespace httplib::body {

namespace json = boost::json;

struct json_body
{
    using value_type = json::value;

    struct writer
    {
        using const_buffers_type = net::const_buffer;

        writer(const http::fields&, value_type const& body);
        ~writer();
        writer(writer&&) noexcept;
        writer& operator=(writer&&) noexcept;

        void init(boost::system::error_code& ec);

        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

    struct reader
    {
        reader(const http::fields&, value_type& body);
        ~reader();
        reader(reader&&) noexcept;
        reader& operator=(reader&&) noexcept;

        void init(boost::optional<std::uint64_t> const& content_length,
                  boost::system::error_code& ec);


        std::size_t put(net::const_buffer const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        class impl;
        std::unique_ptr<impl> impl_;
    };
};
} // namespace httplib::body

#endif
