#pragma once
#include "form_data.hpp"
#include "httplib/config.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>

namespace httplib::body
{

class form_data_body
{
public:
    using value_type = form_data;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields>& h, value_type& b);

        void init(boost::system::error_code& ec)
        {
            ec.clear();
            field_data_index_ = 0;
        }
        inline boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        value_type& body_;
        std::string boundary_;
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

        template<bool isRequest, class Fields>
        reader(http::header<isRequest, Fields>& h, value_type& b);

        inline void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec);

        inline std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);

        void finish(boost::system::error_code& ec)
        {
            ec.clear();
            if (step_ != step::eof)
            {
                ec = http::error::partial_message;
            }
        }

    private:
        value_type& body_;
        std::string_view content_type_;
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
        form_field_data field_data_;
    };
};
} // namespace httplib::body

#include "httplib/body/impl/form_data_body.hpp"