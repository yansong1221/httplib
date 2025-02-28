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


} // namespace detail


class any_body::writer::impl
{
public:
    explicit impl(http::fields& header, any_body::value_type& body)
        : proxy_(create_proxy_writer(header, body))
        , compressor_(compressor::create(compressor::mode::encode, header[http::field::content_encoding]))
    {
    }
    void init(boost::system::error_code& ec) { proxy_->init(ec); }
    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> get(boost::system::error_code& ec)
    {
        if (!compressor_) return proxy_->get(ec);

        compressor_->consume();
        for (;;)
        {
            auto result = proxy_->get(ec);
            if (!result || ec) return result;

            if (!result)
            {
                compressor_->finish();
                auto buffer = compressor_->buffer();
                return {{buffer, false}};
            }

            compressor_->write(net::buffer(result->first), result->second);
            auto buffer = compressor_->buffer();
            if (buffer.size() != 0) return {{buffer, result->second}};
        }
    }

private:
    template<typename... Bodies>
    std::unique_ptr<detail::proxy_writer> create_proxy_writer(http::fields& h, any_body::variant_value<Bodies...>& body)
    {
        return std::visit(
            [&](auto& t) -> std::unique_ptr<detail::proxy_writer>
            {
                using value_type = std::decay_t<decltype(t)>;
                // 提取匹配的 Body 类型
                using body_type = typename detail::match_body<value_type, Bodies...>::type;
                static_assert(!std::is_void_v<body_type>, "No matching Body type found");

                return std::make_unique<detail::impl_proxy_writer<body_type>>(h, t);
            },
            body);
    }

private:
    std::unique_ptr<detail::proxy_writer> proxy_;
    std::unique_ptr<compressor> compressor_;
};

class any_body::reader::impl
{
public:
    impl(http::fields& header, any_body::value_type& body)
        : compressor_(compressor::create(compressor::mode::decode, header[http::field::content_encoding]))
    {
        auto content_type = header[http::field::content_type];
        if (content_type.starts_with("multipart/form-data"))
        {
            proxy_ = create_proxy_reader<form_data_body>(header, body);
        }
        else if (content_type.starts_with("application/json"))
        {
            proxy_ = create_proxy_reader<json_body>(header, body);
        }
        else
        {
            proxy_ = create_proxy_reader<string_body>(header, body);
        }
    }
    void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
    {
        proxy_->init(content_length, ec);
    }
    std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec)
    {
        if (!compressor_) return proxy_->put(buffers, ec);

        compressor_->write(buffers);

        auto decoded_buffer = compressor_->buffer();
        if (decoded_buffer.size() != 0)
        {
            auto bytes = proxy_->put(decoded_buffer, ec);
            compressor_->consume(bytes);
        }
        return buffers.size();
    }
    void finish(boost::system::error_code& ec) { proxy_->finish(ec); }

private:
    template<class Body>
    std::unique_ptr<detail::proxy_reader> create_proxy_reader(http::fields& h, any_body::value_type& body)
    {
        return std::visit(
            [&](auto& t) mutable -> std::unique_ptr<detail::proxy_reader>
            {
                using value_type = std::decay_t<decltype(t)>;
                if constexpr (!std::same_as<value_type, typename Body::value_type>)
                {
                    body = typename Body::value_type {};
                    return create_proxy_reader<Body>(h, body);
                }
                else
                {
                    return std::make_unique<detail::impl_proxy_reader<Body>>(h, t);
                }
            },
            body);
    }

private:
    std::unique_ptr<detail::proxy_reader> proxy_;
    std::unique_ptr<compressor> compressor_;
};

any_body::writer::writer(http::fields& h, value_type& b) : impl_(new any_body::writer::impl(h, b)) { }

any_body::writer::~writer() { delete impl_; }
void any_body::writer::init(boost::system::error_code& ec) { impl_->init(ec); }

boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> any_body::writer::get(
    boost::system::error_code& ec)
{
    return impl_->get(ec);
}

any_body::reader::reader(http::fields& h, value_type& b) : impl_(new any_body::reader::impl(h, b)) { }
any_body::reader::~reader() { delete impl_; }
void any_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
{
    impl_->init(content_length, ec);
}
std::size_t any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
{
    return impl_->put(buffers, ec);
}
void any_body::reader::finish(boost::system::error_code& ec) { impl_->finish(ec); }


} // namespace httplib::body
