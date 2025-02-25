#pragma once

namespace httplib::body
{

class any_body::proxy_writer
{
public:
    virtual ~proxy_writer() = default;
    virtual void init(boost::system::error_code& ec) = 0;
    virtual inline boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> get(
        boost::system::error_code& ec) = 0;
};
class any_body::proxy_reader
{
public:
    virtual ~proxy_reader() = default;
    virtual void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) = 0;
    virtual std::size_t put(any_body::reader::const_buffers_type const& buffers, boost::system::error_code& ec) = 0;
    virtual void finish(boost::system::error_code& ec) = 0;
};

namespace detail
{
// 辅助模板：匹配 body_value_type 对应的 Body 类型
template<typename T, typename... Bodies>
struct match_body;

template<typename T, typename Body, typename... Bodies>
struct match_body<T, Body, Bodies...>
{
    using type =
        std::conditional_t<std::is_same_v<T, typename Body::value_type>, Body, typename match_body<T, Bodies...>::type>;
};

template<typename T>
struct match_body<T>
{
    using type = void; // 无匹配时返回 void
};


template<class Body>
class impl_proxy_writer : public any_body::proxy_writer
{
public:
    template<bool isRequest, class Fields>
    impl_proxy_writer(http::header<isRequest, Fields>& h, typename Body::value_type& b) : writer_(h, b)
    {
    }
    void init(boost::system::error_code& ec) override { writer_.init(ec); };
    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> get(boost::system::error_code& ec) override
    {
        return writer_.get(ec);
    };

private:
    typename Body::writer writer_;
};


template<class Body>
class impl_proxy_reader : public any_body::proxy_reader
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
    std::size_t put(any_body::reader::const_buffers_type const& buffers, boost::system::error_code& ec) override
    {
        return reader_.put(buffers, ec);
    }
    void finish(boost::system::error_code& ec) override { reader_.finish(ec); }

private:
    typename Body::reader reader_;
};
} // namespace detail
template<typename... Bodies>
template<typename Body>
bool any_body::variant_value<Bodies...>::is_body_type() const
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

template<typename... Bodies>
template<bool isRequest, class Fields>
std::unique_ptr<any_body::proxy_writer> any_body::variant_value<Bodies...>::create_proxy_writer(
    http::header<isRequest, Fields>& h)
{
    return std::visit(
        [&](auto& t) -> std::unique_ptr<proxy_writer>
        {
            using value_type = std::decay_t<decltype(t)>;
            // 提取匹配的 Body 类型
            using body_type = typename detail::match_body<value_type, Bodies...>::type;
            static_assert(!std::is_void_v<body_type>, "No matching Body type found");

            return std::make_unique<detail::impl_proxy_writer<body_type>>(h, t);
        },
        *this);
}
template<typename... Bodies>
template<class Body, bool isRequest, class Fields>
std::unique_ptr<any_body::proxy_reader> any_body::variant_value<Bodies...>::create_proxy_reader(
    http::header<isRequest, Fields>& h)
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
                return std::make_unique<detail::impl_proxy_reader<Body>>(h, t);
            }
        },
        *this);
}

template<bool isRequest, class Fields>
any_body::writer::writer(http::header<isRequest, Fields>& h, value_type& b)
{
    proxy_ = b.create_proxy_writer(h);
}
void any_body::writer::init(boost::system::error_code& ec) { proxy_->init(ec); }

boost::optional<std::pair<httplib::body::any_body::writer::const_buffers_type, bool>> any_body::writer::get(
    boost::system::error_code& ec)
{
    return proxy_->get(ec);
}

template<bool isRequest, class Fields>
any_body::reader::reader(http::header<isRequest, Fields>& h, value_type& b)
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
void any_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
{
    return proxy_->init(content_length, ec);
}
std::size_t any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
{
    return proxy_->put(buffers, ec);
}
void any_body::reader::finish(boost::system::error_code& ec) { return proxy_->finish(ec); }
} // namespace httplib::body