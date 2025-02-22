#pragma once
#include "httplib/config.hpp"
#include <boost/beast/core/file.hpp>
#include <boost/beast/http/message.hpp>

namespace httplib::body
{
struct range_body
{
    struct range_data
    {
        using range_type = std::pair<int64_t, int64_t>;

        std::vector<range_type> ranges;
        beast::file file;
    };

    using value_type = range_data;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields>& h, value_type& b);

        void init(boost::system::error_code& ec) { }
        inline boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec) { }
    };
    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        reader(http::header<isRequest, Fields>& h, value_type& b)
        {
        }

        inline void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) { }

        inline std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec) { }

        void finish(boost::system::error_code& ec) { ec.clear(); }

    private:
        value_type& body_;
    };
};
} // namespace httplib::body