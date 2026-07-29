#pragma once
#include "httplib/config.hpp"
#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/detail/config.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/optional.hpp>
#include <cstdint>

namespace httplib::body {

struct buffer_body
{
    struct value_type
    {
        void* data = nullptr;
        std::size_t size = 0;
        bool more = true;
    };

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields>&, value_type& b)
            : body_(b)
        {
        }
        explicit reader(http::fields&, value_type& b);

        void init(boost::optional<std::uint64_t> const&, boost::system::error_code& ec);
        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        value_type& body_;
    };

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        explicit writer(http::header<isRequest, Fields> const&, value_type const& b)
            : body_(b)
        {
        }
        explicit writer(http::fields const&, value_type const& b);

        void init(boost::system::error_code& ec);
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        bool toggle_ = false;
        value_type const& body_;
    };
};

} // namespace httplib::body
