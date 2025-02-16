#pragma once
#include "form_data.hpp"
#include "httplib/config.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http/message.hpp>

namespace httplib {

class form_data_body {
public:
    using value_type = httplib::form_data;

    class writer {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields> &h, value_type &b)
            : body_(b), boundary_(generate_boundary()) {
            h.set(http::field::content_type,
                  std::format("multipart/form-data; boundary={}", boundary_));
        }

        void init(boost::system::error_code &ec);
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code &ec);

    private:
        static std::string generate_boundary();

    private:
        value_type &body_;
        std::string boundary_;
        int field_data_index_ = 0;
        beast::flat_buffer buffer_;

        enum class step {
            header,
            content,
            content_end,
            eof
        };
        step step_ = step::header;
    };

    //--------------------------------------------------------------------------

    class reader {

    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields> &h, value_type &b) : body_(b) {
            content_type_ = h[http::field::content_type];
        }

        void init(boost::optional<std::uint64_t> const &content_length,
                  boost::system::error_code &ec);

        std::size_t put(const_buffers_type const &buffers, boost::system::error_code &ec);

        void finish(boost::system::error_code &ec);

    private:
        value_type &body_;
        std::string_view content_type_;
        std::string boundary_;
        enum class step {
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
} // namespace httplib