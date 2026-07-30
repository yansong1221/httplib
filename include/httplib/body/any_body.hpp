#pragma once
#include "httplib/body/empty_body.hpp"
#include "httplib/body/file_body.hpp"
#include "httplib/body/form_data_body.hpp"
#include "httplib/body/json_body.hpp"
#include "httplib/body/query_params_body.hpp"
#include "httplib/body/string_body.hpp"

namespace httplib::body {
struct any_body
{
    template<typename... Bodies>
    class variant_value : public std::variant<typename Bodies::value_type...>
    {
    public:
        using std::variant<typename Bodies::value_type...>::variant;

        template<typename Body>
        typename Body::value_type& as() &
        {
            return std::get<typename Body::value_type>(*this);
        }

        template<typename Body>
        const typename Body::value_type& as() const&
        {
            return std::get<typename Body::value_type>(*this);
        }

        template<typename Body>
        bool is_body_type() const
        {
            return std::holds_alternative<typename Body::value_type>(*this);
        }
    };

    using value_type = variant_value<empty_body,
                                     string_body,
                                     json_body,
                                     form_data_body,
                                     file_body,
                                     query_params_body>;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        explicit writer(http::header<isRequest, Fields>& h, value_type& b)
            : writer(static_cast<http::fields&>(h), b)
        {
        }
        explicit writer(http::fields& h, value_type& b);
        virtual ~writer();

        void init(boost::system::error_code& ec);
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        class impl;
        std::unique_ptr<impl> impl_;
    };
    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields>& h, value_type& b)
            : reader(static_cast<http::fields&>(h), b)
        {
        }
        explicit reader(http::fields& h, value_type& b);

        virtual ~reader();

        void init(boost::optional<std::uint64_t> const& content_length,
                  boost::system::error_code& ec);
        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        class impl;
        std::unique_ptr<impl> impl_;
    };
};


} // namespace httplib::body