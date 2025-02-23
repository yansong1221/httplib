#pragma once
#include "file_body.hpp"
#include "form_data_body.hpp"
#include "json_body.hpp"

namespace httplib::body
{


struct any_body
{
    using const_buffers_type = net::const_buffer;
    class writer;
    class reader;

private:
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

    class proxy_writer
    {
    public:
        virtual ~proxy_writer() = default;
        virtual void init(boost::system::error_code& ec) = 0;
        virtual inline boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec) = 0;
    };
    class proxy_reader
    {
    public:
        virtual ~proxy_reader() = default;
        virtual void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) = 0;
        virtual std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec) = 0;
        virtual void finish(boost::system::error_code& ec) = 0;
    };

    template<class Body>
    class impl_proxy_writer : public proxy_writer
    {
    public:
        template<bool isRequest, class Fields>
        impl_proxy_writer(http::header<isRequest, Fields>& h, typename Body::value_type& b) : writer_(h, b)
        {
        }
        void init(boost::system::error_code& ec) override { writer_.init(ec); };
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec) override
        {
            return writer_.get(ec);
        };

    private:
        typename Body::writer writer_;
    };


    template<class Body>
    class impl_proxy_reader : public proxy_reader
    {
    public:
        template<bool isRequest, class Fields>
        impl_proxy_reader(http::header<isRequest, Fields>& h, typename Body::value_type& b) : reader_(h, b)
        {
        }
        void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) override
        {
            reader_.init(content_length, ec);
        }
        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec) override
        {
            return reader_.put(buffers, ec);
        }
        void finish(boost::system::error_code& ec) override { reader_.finish(ec); }

    private:
        typename Body::reader reader_;
    };


    template<typename... Bodies>
    class variant_value : public std::variant<typename Bodies::value_type...>
    {
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
            return std::get<typename Body::value_type>(*this);
        }
        template<class Body>
        const typename Body::value_type& as() const&
        {
            return std::get<typename Body::value_type>(*this);
        }

    private:
        template<bool isRequest, class Fields>
        std::unique_ptr<proxy_writer> create_proxy_writer(http::header<isRequest, Fields>& h)
        {
            return std::visit(
                [&](auto& t) -> std::unique_ptr<proxy_writer>
                {
                    using value_type = std::decay_t<decltype(t)>;
                    // 提取匹配的 Body 类型
                    using body_type = typename match_body<value_type, Bodies...>::type;
                    static_assert(!std::is_void_v<body_type>, "No matching Body type found");

                    return std::make_unique<impl_proxy_writer<body_type>>(h, t);
                },
                *this);
        }
        template<class Body, bool isRequest, class Fields>
        std::unique_ptr<proxy_reader> create_proxy_reader(http::header<isRequest, Fields>& h)
        {
            return std::visit(
                [&](auto& t) mutable -> std::unique_ptr<proxy_reader>
                {
                    using value_type = std::decay_t<decltype(t)>;
                    if constexpr (!std::same_as<value_type, typename Body::value_type>)
                    {
                        *this = typename Body::value_type {};
                        return this->create_proxy_reader<Body>(h);
                    }
                    else
                    {
                        return std::make_unique<impl_proxy_reader<Body>>(h, t);
                    }
                },
                *this);
        }
        friend class any_body::writer;
        friend class any_body::reader;
    };

public:
    using value_type = variant_value<http::empty_body, http::string_body, json_body, form_data_body, file_body>;

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

    public:
        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields>& h, value_type& b)
        {
            proxy_ = b.create_proxy_writer(h);

            if constexpr (!isRequest)
            {
                if (b.is_body_type<http::string_body>())
                {
                    h.set(http::field::content_length, std::to_string(b.as<http::string_body>().length()));
                }
                else if (b.is_body_type<file_body>())
                {
                    boost::system::error_code ec;
                    h.set(http::field::content_length, std::to_string(b.as<file_body>().size(ec)));
                }
            }
        }

        void init(boost::system::error_code& ec) { proxy_->init(ec); }
        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec)
        {
            return proxy_->get(ec);
        }

    private:
        std::unique_ptr<proxy_writer> proxy_;
    };
    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

    private:
        template<class Body, bool isRequest, class Fields>
        std::unique_ptr<proxy_reader> create_proxy_reader(http::header<isRequest, Fields>& h, value_type& b)
        {
            if (!std::holds_alternative<typename Body::value_type>(b))
            {
                b = typename Body::value_type {};
            }
            return std::make_unique<impl_proxy_reader<Body>>(h, std::get<typename Body::value_type>(b));
        }

    public:
        template<bool isRequest, class Fields>
        reader(http::header<isRequest, Fields>& h, value_type& b)
        {
            auto content_type = h[http::field::content_type];
            if (content_type.starts_with("multipart/form-data"))
            {
                proxy_ = b.create_proxy_reader<form_data_body>(h);
            }
            else if (content_type.starts_with("application/json"))
            {
                proxy_ = b.create_proxy_reader<json_body>(h);
            }
            else
            {
                proxy_ = b.create_proxy_reader<http::string_body>(h);
            }
        }

        void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
        {
            return proxy_->init(content_length, ec);
        }

        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec)
        {
            return proxy_->put(buffers, ec);
        }
        void finish(boost::system::error_code& ec) { return proxy_->finish(ec); }

    private:
        std::unique_ptr<proxy_reader> proxy_;
    };
};
} // namespace httplib::body