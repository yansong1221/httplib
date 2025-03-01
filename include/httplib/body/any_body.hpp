#pragma once
#include "httplib/body/empty_body.hpp"
#include "httplib/body/file_body.hpp"
#include "httplib/body/form_data_body.hpp"
#include "httplib/body/json_body.hpp"
#include "httplib/body/query_params_body.hpp"
#include "httplib/body/string_body.hpp"

namespace httplib::body
{
struct any_body
{
    // 辅助模板：匹配 body_value_type 对应的 Body 类型
    template<typename T, typename... Bodies>
    struct match_body;

    template<typename T, typename Body, typename... Bodies>
    struct match_body<T, Body, Bodies...>
    {
        using type = std::
            conditional_t<std::is_same_v<T, typename Body::value_type>, Body, typename match_body<T, Bodies...>::type>;
    };

    template<typename T>
    struct match_body<T>
    {
        using type = void; // 无匹配时返回 void
    };

    template<typename... Bodies>
    class variant_value : public std::variant<typename Bodies::value_type...>
    {
    public:
        using std::variant<typename Bodies::value_type...>::variant;

    public:
        template<typename Body>
        bool is_body_type() const
        {
            return std::visit(
                [](auto& t)
                {
                    using value_type = std::decay_t<decltype(t)>;
                    if constexpr (std::same_as<typename Body::value_type, value_type>)
                        return true;
                    else
                        return false;
                },
                *this);
        }

        template<class Body>
        typename Body::value_type& as() &
        {
            using body_type = typename match_body<typename Body::value_type, Bodies...>::type;
            static_assert(!std::is_void_v<body_type>, "No matching Body type found");
            return std::get<typename Body::value_type>(*this);
        }

        template<class Body>
        const typename Body::value_type& as() const&
        {
            using body_type = typename match_body<typename Body::value_type, Bodies...>::type;
            static_assert(!std::is_void_v<body_type>, "No matching Body type found");
            return std::get<typename Body::value_type>(*this);
        }
    };

    using value_type = variant_value<empty_body, string_body, json_body, form_data_body, file_body, query_params_body>;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        explicit writer(http::header<isRequest, Fields>& h, value_type& b) : writer(static_cast<http::fields&>(h), b)
        {
        }
        explicit writer(http::fields& h, value_type& b);
        virtual ~writer();

        void init(boost::system::error_code& ec);
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

    private:
        class impl;
        impl* impl_ = nullptr;
    };
    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields>& h, value_type& b) : reader(static_cast<http::fields&>(h), b)
        {
        }
        explicit reader(http::fields& h, value_type& b);

        virtual ~reader();

        void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec);
        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        class impl;
        impl* impl_ = nullptr;
    };
};


} // namespace httplib::body