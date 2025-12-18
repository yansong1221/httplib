#include "httplib/body/any_body.hpp"
#include "compressor.hpp"

namespace httplib::body {
namespace detail {

class proxy_writer
{
public:
    using ptr = std::unique_ptr<proxy_writer>;

    virtual ~proxy_writer()                          = default;
    virtual void init(boost::system::error_code& ec) = 0;
    virtual inline boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
    get(boost::system::error_code& ec) = 0;
};
class proxy_reader
{
public:
    using ptr = std::unique_ptr<proxy_reader>;

    virtual ~proxy_reader()                                = default;
    virtual void init(boost::optional<std::uint64_t> const& content_length,
                      boost::system::error_code& ec)       = 0;
    virtual std::size_t put(any_body::reader::const_buffers_type const& buffers,
                            boost::system::error_code& ec) = 0;
    virtual void finish(boost::system::error_code& ec)     = 0;
};


template<class Body>
class proxy_writer_impl : public proxy_writer
{
public:
    proxy_writer_impl(http::fields& h, typename Body::value_type& b)
        : writer_(h, b)
    {
    }
    void init(boost::system::error_code& ec) override { writer_.init(ec); };
    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
    get(boost::system::error_code& ec) override
    {
        return writer_.get(ec);
    };

private:
    typename Body::writer writer_;
};


template<class Body>
class proxy_reader_impl : public proxy_reader
{
public:
    proxy_reader_impl(http::fields& h, typename Body::value_type& b)
        : reader_(h, b)
    {
    }
    void init(boost::optional<std::uint64_t> const& content_length,
              boost::system::error_code& ec) override
    {
        reader_.init(content_length, ec);
    }
    std::size_t put(any_body::reader::const_buffers_type const& buffers,
                    boost::system::error_code& ec) override
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
        : header_(header)
        , body_(body)
    {
    }
    void init(boost::system::error_code& ec)
    {
        auto content_encoding = header_[http::field::content_encoding];

        proxy_      = create_proxy_writer(header_, body_);
        compressor_ = compressor_factory::instance().create(content_encoding);

        if (compressor_)
            compressor_->init(compressor::mode::encode);
        proxy_->init(ec);
    }
    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
    get(boost::system::error_code& ec)
    {
        if (!compressor_)
            return proxy_->get(ec);

        compressor_->consume_all();
        for (;;) {
            auto result = proxy_->get(ec);
            if (!result || ec)
                return result;

            if (!result) {
                compressor_->finish();
                auto buffer = compressor_->buffer();
                return {{buffer, false}};
            }

            compressor_->write(net::buffer(result->first), result->second);
            auto buffer = compressor_->buffer();
            if (buffer.size() != 0)
                return {{buffer, result->second}};
        }
    }

private:
    template<typename... Bodies>
    auto create_proxy_writer(http::fields& h, any_body::variant_value<Bodies...>& body)
    {
        return std::visit(
            [&](auto& t) -> detail::proxy_writer::ptr {
                using value_type = std::decay_t<decltype(t)>;
                // 提取匹配的 Body 类型
                using body_type = typename any_body::match_body<value_type, Bodies...>::type;
                static_assert(!std::is_void_v<body_type>, "No matching Body type found");

                using T = detail::proxy_writer_impl<body_type>;

                return std::make_unique<T>(h, t);
            },
            body);
    }

private:
    http::fields& header_;
    any_body::value_type& body_;

    detail::proxy_writer::ptr proxy_;
    compressor::ptr compressor_;
};

class any_body::reader::impl
{
public:
    impl(http::fields& header, any_body::value_type& body)
        : header_(header)
        , body_(body)
    {
    }
    void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
    {
        auto content_type     = header_[http::field::content_type];
        auto content_encoding = header_[http::field::content_encoding];

        if (content_type.starts_with("multipart/form-data")) {
            proxy_ = create_proxy_reader<form_data_body>(header_, body_);
        }
        else if (content_type.starts_with("application/json")) {
            proxy_ = create_proxy_reader<json_body>(header_, body_);
        }
        else if (content_type.starts_with("application/x-www-form-urlencoded")) {
            proxy_ = create_proxy_reader<query_params_body>(header_, body_);
        }
        else {
            proxy_ = create_proxy_reader<string_body>(header_, body_);
        }
        compressor_ = compressor_factory::instance().create(content_encoding);
        if (compressor_)
            compressor_->init(compressor::mode::decode);
        proxy_->init(content_length, ec);
    }
    std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec)
    {
        if (!compressor_)
            return proxy_->put(buffers, ec);

        compressor_->write(buffers);

        auto decoded_buffer = compressor_->buffer();
        if (decoded_buffer.size() != 0) {
            auto bytes = proxy_->put(decoded_buffer, ec);
            compressor_->consume(bytes);
        }
        return buffers.size();
    }
    void finish(boost::system::error_code& ec)
    {
        if (!compressor_)
            return proxy_->finish(ec);

        compressor_->finish();
        auto decoded_buffer = compressor_->buffer();
        if (decoded_buffer.size() != 0) {
            proxy_->put(decoded_buffer, ec);
        }
        return proxy_->finish(ec);
    }

private:
    template<class Body>
    auto create_proxy_reader(http::fields& h, any_body::value_type& b)
    {
        return std::visit(
            [&](auto& t) mutable -> detail::proxy_reader::ptr {
                using value_type = std::decay_t<decltype(t)>;
                if constexpr (!std::same_as<value_type, typename Body::value_type>) {
                    b = typename Body::value_type {};
                    return create_proxy_reader<Body>(h, b);
                }
                else {
                    using T = detail::proxy_reader_impl<Body>;
                    return  std::make_unique<T>(h, t);
                }
            },
            b);
    }

private:
    http::fields& header_;
    any_body::value_type& body_;

    detail::proxy_reader::ptr proxy_;
    compressor::ptr compressor_;
};

any_body::writer::writer(http::fields& h, value_type& b)
    : impl_( std::make_unique<any_body::writer::impl>(h, b))
{
}

any_body::writer::~writer()
{
}
void any_body::writer::init(boost::system::error_code& ec)
{
    impl_->init(ec);
}

boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
any_body::writer::get(boost::system::error_code& ec)
{
    return impl_->get(ec);
}

any_body::reader::reader(http::fields& h, value_type& b)
    : impl_( std::make_unique<any_body::reader::impl>(h, b))
{
}
any_body::reader::~reader()
{
}
void any_body::reader::init(boost::optional<std::uint64_t> const& content_length,
                            boost::system::error_code& ec)
{
    impl_->init(content_length, ec);
}
std::size_t any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
{
    return impl_->put(buffers, ec);
}
void any_body::reader::finish(boost::system::error_code& ec)
{
    impl_->finish(ec);
}


} // namespace httplib::body
