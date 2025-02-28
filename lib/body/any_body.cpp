#include "httplib/body/any_body.hpp"

#include "compressor.hpp"

namespace httplib::body
{
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

class proxy_writer
{
public:
    virtual ~proxy_writer() = default;
    virtual void init(boost::system::error_code& ec) = 0;
    virtual inline boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> get(
        boost::system::error_code& ec) = 0;
};
class proxy_reader
{
public:
    virtual ~proxy_reader() = default;
    virtual void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) = 0;
    virtual std::size_t put(any_body::reader::const_buffers_type const& buffers, boost::system::error_code& ec) = 0;
    virtual void finish(boost::system::error_code& ec) = 0;
};


template<class Body>
class impl_proxy_writer : public proxy_writer
{
public:
    impl_proxy_writer(http::fields& h, typename Body::value_type& b) : writer_(h, b) { }
    void init(boost::system::error_code& ec) override { writer_.init(ec); };
    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> get(boost::system::error_code& ec) override
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
    impl_proxy_reader(http::fields& h, typename Body::value_type& b) : reader_(h, b) { }
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


template<class Body>
std::unique_ptr<proxy_reader> create_proxy_reader(http::fields& h, any_body::value_type& body)
{
    return std::visit(
        [&](auto& t) mutable -> std::unique_ptr<proxy_reader>
        {
            using value_type = std::decay_t<decltype(t)>;
            if constexpr (!std::same_as<value_type, typename Body::value_type>)
            {
                body = typename Body::value_type {};
                return create_proxy_reader<Body>(h, body);
            }
            else
            {
                return std::make_unique<impl_proxy_reader<Body>>(h, t);
            }
        },
        body);
}
template<typename... Bodies>
std::unique_ptr<proxy_writer> create_proxy_writer(http::fields& h, any_body::variant_value<Bodies...>& body)
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
        body);
}

} // namespace detail


class any_body::writer::impl
{
public:
    std::unique_ptr<detail::proxy_writer> proxy_;
    std::unique_ptr<compressor> compressor_;
};

class any_body::reader::impl
{
public:
    std::unique_ptr<detail::proxy_reader> proxy_;
    std::unique_ptr<compressor> compressor_;
};

any_body::writer::~writer() { delete impl_; }
void any_body::writer::init(boost::system::error_code& ec)
{
    auto proxy = detail::create_proxy_writer(header_, body_);
    proxy->init(ec);
    if (ec) return;

    impl_ = new any_body::writer::impl();
    impl_->proxy_ = std::move(proxy);
    impl_->compressor_ = compressor::create(compressor::mode::encode, header_[http::field::content_encoding]);
}

boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> any_body::writer::get(
    boost::system::error_code& ec)
{
    if (!impl_->compressor_) return impl_->proxy_->get(ec);

    impl_->compressor_->consume();
    for (;;)
    {
        auto result = impl_->proxy_->get(ec);
        if (!result || ec) return result;

        if (!result)
        {
            impl_->compressor_->finish();
            auto buffer = impl_->compressor_->buffer();
            return {{buffer, false}};
        }

        impl_->compressor_->write(net::buffer(result->first), result->second);
        auto buffer = impl_->compressor_->buffer();
        if (buffer.size() != 0) return {{buffer, result->second}};
    }
}

any_body::reader::~reader() { delete impl_; }
void any_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
{
    impl_ = new any_body::reader::impl();
    auto content_type = header_[http::field::content_type];
    if (content_type.starts_with("multipart/form-data"))
    {
        impl_->proxy_ = detail::create_proxy_reader<form_data_body>(header_, body_);
    }
    else if (content_type.starts_with("application/json"))
    {
        impl_->proxy_ = detail::create_proxy_reader<json_body>(header_, body_);
    }
    else
    {
        impl_->proxy_ = detail::create_proxy_reader<string_body>(header_, body_);
    }

    return impl_->proxy_->init(content_length, ec);
}
std::size_t any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
{
    return impl_->proxy_->put(buffers, ec);
}
void any_body::reader::finish(boost::system::error_code& ec)
{
    if (impl_) return impl_->proxy_->finish(ec);
}


} // namespace httplib::body
