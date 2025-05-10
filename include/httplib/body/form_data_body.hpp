#pragma once
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/fields.hpp>

namespace httplib::body {

class form_data_body
{
public:
    using value_type = form_data;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;


        writer(http::fields const& h, value_type& b);

        void init(boost::system::error_code& ec);
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        value_type& body_;
        int field_data_index_ = 0;
        beast::flat_buffer buffer_;

        enum class step
        {
            header,
            content,
            content_end,
            eof
        };
        step step_ = step::header;
    };

    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

        reader(http::fields const& h, value_type& b);

        void init(boost::optional<std::uint64_t> const& content_length,
                  boost::system::error_code& ec);

        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);

        void finish(boost::system::error_code& ec);

    private:
        value_type& body_;
        std::string content_type_;
        std::string boundary_;
        enum class step
        {
            boundary_line,
            boundary_header,
            boundary_content,
            finshed,
            eof
        };
        step step_ = step::boundary_line;
        form_data::field field_data_;
    };
};
} // namespace httplib::body
