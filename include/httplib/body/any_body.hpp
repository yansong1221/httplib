#pragma once
#include "empty_body.hpp"
#include "file_body.hpp"
#include "form_data_body.hpp"
#include "httplib/config.hpp"
#include "json_body.hpp"
#include "string_body.hpp"
#include <boost/beast/http/message.hpp>

namespace httplib::body
{
class compressor;
struct any_body
{
    class writer;
    class reader;

    class proxy_writer;
    class proxy_reader;

    template<typename... Bodies>
    class variant_value : public std::variant<typename Bodies::value_type...>
    {
        using std::variant<typename Bodies::value_type...>::variant;

    public:
        template<typename Body>
        bool is_body_type() const;

        template<class Body>
        typename Body::value_type& as() &
        {
            return std::get<typename Body::value_type>(*this);
        }

        template<class Body>
        const typename Body::value_type& as() const&
        {
            return std::get<typename Body::value_type>(*this);
        }

    private:
        inline std::unique_ptr<proxy_writer> create_proxy_writer(http::fields& h);
        template<class Body>
        inline std::unique_ptr<proxy_reader> create_proxy_reader(http::fields& h);
        friend class any_body::writer;
        friend class any_body::reader;
    };

    using value_type = variant_value<empty_body, string_body, json_body, form_data_body, file_body>;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields>& h, value_type& b);

        void init(boost::system::error_code& ec);
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        std::unique_ptr<proxy_writer> proxy_;
        std::unique_ptr<compressor> compressor_;
    };
    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        reader(http::header<isRequest, Fields>& h, value_type& b);

        void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec);
        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        std::unique_ptr<proxy_reader> proxy_;
    };
};


} // namespace httplib::body

#include "httplib/body/impl/any_body.hpp"
