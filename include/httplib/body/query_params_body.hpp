#pragma once
#include "httplib/config.hpp"
#include "httplib/html/query_params.hpp"
#include <boost/beast/http/fields.hpp>
#include <string>
#include <unordered_map>

namespace httplib::body {
struct query_params_body
{
    using value_type = html::query_params;

    struct writer
    {
        using const_buffers_type = net::const_buffer;

        writer(const http::fields&, value_type const& body);

        void init(boost::system::error_code& ec);

        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        value_type const& body_;
        std::string buffer_;
    };

    struct reader
    {
        reader(const http::fields&, value_type& body);
        void init(boost::optional<std::uint64_t> const& content_length,
                  boost::system::error_code& ec);


        std::size_t put(net::const_buffer const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        value_type& body_;
        std::string buffer_;
    };
};
} // namespace httplib::body