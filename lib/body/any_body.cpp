#include "body/any_body.hpp"
#include "compress/compressor.hpp"
#include <tuple>
#include <variant>

using namespace httplib::compress;

namespace httplib::body
{
    namespace detail
    {

        template <typename T, typename... Bodies>
        struct match_body;

        template <typename T, typename Body, typename... Bodies>
        struct match_body<T, Body, Bodies...>
        {
            using type = std::conditional_t<std::is_same_v<T, typename Body::value_type>,
                                            Body,
                                            typename match_body<T, Bodies...>::type>;
        };

        template <typename T>
        struct match_body<T>
        {
            using type = void;
        };

        template <typename... Bodies>
        struct variant_from_tuple;

        template <typename... Bodies>
        struct variant_from_tuple<std::tuple<Bodies...>>
        {
            using writer_t = std::variant<std::monostate, typename Bodies::writer...>;
            using reader_t = std::variant<std::monostate, typename Bodies::reader...>;
        };

        using writer_variant = variant_from_tuple<any_body::body_types>::writer_t;
        using reader_variant = variant_from_tuple<any_body::body_types>::reader_t;

    } // namespace detail

    struct writer_holder
    {
        detail::writer_variant v;

        void
        init(boost::system::error_code& ec)
        {
            std::visit(
                [&](auto& w)
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(w)>, std::monostate>)
                    {
                        w.init(ec);
                    }
                },
                v);
        }

        boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
        get(boost::system::error_code& ec)
        {
            return std::visit(
                [&](auto& w) -> boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(w)>, std::monostate>)
                    {
                        return w.get(ec);
                    }
                    return boost::none;
                },
                v);
        }

        template <typename Body>
        void
        emplace(http::fields& h, typename Body::value_type& b)
        {
            v.template emplace<typename Body::writer>(h, b);
        }
    };

    struct reader_holder
    {
        detail::reader_variant v;

        void
        init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
        {
            std::visit(
                [&](auto& r)
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(r)>, std::monostate>)
                    {
                        r.init(content_length, ec);
                    }
                },
                v);
        }

        std::size_t
        put(any_body::reader::const_buffers_type const& buffers, boost::system::error_code& ec)
        {
            return std::visit(
                [&](auto& r)
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(r)>, std::monostate>)
                    {
                        return r.put(buffers, ec);
                    }
                    return std::size_t(0);
                },
                v);
        }

        void
        finish(boost::system::error_code& ec)
        {
            std::visit(
                [&](auto& r)
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(r)>, std::monostate>)
                    {
                        r.finish(ec);
                    }
                },
                v);
        }

        template <typename Body>
        void
        emplace(http::fields& h, typename Body::value_type& b)
        {
            v.template emplace<typename Body::reader>(h, b);
        }
    };

    class any_body::writer::impl
    {
      public:
        explicit impl(http::fields& header, any_body::value_type& body) : header_(header), body_(body) {}

        void
        init(boost::system::error_code& ec)
        {
            auto content_encoding = header_[http::field::content_encoding];

            create_writer(header_, body_);
            compressor_ = compressor_factory::instance().create(content_encoding);

            if (compressor_)
            {
                compressor_->init(compressor::mode::encode);
            }
            proxy_.init(ec);
        }

        boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
        get(boost::system::error_code& ec)
        {
            if (!compressor_)
            {
                return proxy_.get(ec);
            }

            compressor_->consume_all();
            for (;;)
            {
                auto result = proxy_.get(ec);
                if (ec)
                {
                    return boost::none;
                }
                if (!result)
                {
                    compressor_->finish();
                    auto buffer = compressor_->buffer();
                    if (buffer.size() != 0)
                    {
                        return {
                            { buffer, false }
                        };
                    }
                    return boost::none;
                }

                compressor_->write(net::buffer(result->first), result->second);
                auto buffer = compressor_->buffer();
                if (buffer.size() != 0)
                {
                    return {
                        { buffer, result->second }
                    };
                }
            }
        }

      private:
        template <typename... Bodies>
        void
        create_writer(http::fields& h, any_body::variant_value<Bodies...>& body)
        {
            std::visit(
                [&](auto& t)
                {
                    using value_type = std::decay_t<decltype(t)>;
                    using body_type = typename detail::match_body<value_type, Bodies...>::type;
                    static_assert(!std::is_void_v<body_type>, "No matching Body type found");

                    proxy_.template emplace<body_type>(h, t);
                },
                body);
        }

      private:
        http::fields& header_;
        any_body::value_type& body_;

        writer_holder proxy_;
        compressor::ptr compressor_;
    };

    class any_body::reader::impl
    {
      public:
        impl(http::fields& header, any_body::value_type& body) : header_(header), body_(body) {}

        void
        init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
        {
            auto content_type = header_[http::field::content_type];
            auto content_encoding = header_[http::field::content_encoding];

            if (!std::holds_alternative<body::empty_body::value_type>(body_))
            {
                create_reader(body_);
            }
            else if (content_type.starts_with("multipart/form-data"))
            {
                create_reader<form_data_body>();
            }
            else if (content_type.starts_with("application/json"))
            {
                create_reader<json_body>();
            }
            else if (content_type.starts_with("application/x-www-form-urlencoded"))
            {
                create_reader<query_params_body>();
            }
            else
            {
                create_reader<string_body>();
            }
            compressor_ = compressor_factory::instance().create(content_encoding);
            if (compressor_)
            {
                compressor_->init(compressor::mode::decode);
            }
            proxy_.init(content_length, ec);
        }

        std::size_t
        put(const_buffers_type const& buffers, boost::system::error_code& ec)
        {
            if (!compressor_)
            {
                return proxy_.put(buffers, ec);
            }

            compressor_->write(buffers);

            auto decoded_buffer = compressor_->buffer();
            while (decoded_buffer.size() != 0 && !ec)
            {
                auto bytes = proxy_.put(decoded_buffer, ec);
                compressor_->consume(bytes);
                if (ec == http::error::need_more && bytes > 0)
                {
                    ec = {};
                    decoded_buffer = compressor_->buffer();
                    continue;
                }
                decoded_buffer = compressor_->buffer();
            }
            return buffers.size();
        }

        void
        finish(boost::system::error_code& ec)
        {
            if (!compressor_)
            {
                return proxy_.finish(ec);
            }

            compressor_->finish();
            auto decoded_buffer = compressor_->buffer();
            while (decoded_buffer.size() != 0 && !ec)
            {
                auto bytes = proxy_.put(decoded_buffer, ec);
                if (ec == http::error::need_more)
                {
                    ec = {};
                    decoded_buffer = net::const_buffer(static_cast<char const*>(decoded_buffer.data()) + bytes,
                                                       decoded_buffer.size() - bytes);
                    continue;
                }
                decoded_buffer = net::const_buffer(static_cast<char const*>(decoded_buffer.data()) + bytes,
                                                   decoded_buffer.size() - bytes);
            }
            if (!ec)
            {
                proxy_.finish(ec);
            }
        }

      private:
        template <typename... Bodies>
        void
        create_reader(any_body::variant_value<Bodies...>& body)
        {
            std::visit(
                [this](auto& t)
                {
                    using value_type = std::decay_t<decltype(t)>;
                    using body_type = typename detail::match_body<value_type, Bodies...>::type;
                    static_assert(!std::is_void_v<body_type>, "No matching Body type found");

                    proxy_.template emplace<body_type>(header_, t);
                },
                body);
        }

        template <class Body>
        void
        create_reader()
        {
            if (!std::holds_alternative<typename Body::value_type>(body_))
            {
                body_ = typename Body::value_type {};
            }

            proxy_.template emplace<Body>(header_, std::get<typename Body::value_type>(body_));
        }

      private:
        http::fields& header_;
        any_body::value_type& body_;

        reader_holder proxy_;
        compressor::ptr compressor_;
    };

    any_body::writer::writer(http::fields& h, value_type& b) : impl_(std::make_unique<any_body::writer::impl>(h, b)) {}

    any_body::writer::~writer() {}
    void
    any_body::writer::init(boost::system::error_code& ec)
    {
        impl_->init(ec);
    }

    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
    any_body::writer::get(boost::system::error_code& ec)
    {
        return impl_->get(ec);
    }

    any_body::reader::reader(http::fields& h, value_type& b) : impl_(std::make_unique<any_body::reader::impl>(h, b)) {}
    any_body::reader::~reader() {}
    void
    any_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
    {
        impl_->init(content_length, ec);
    }
    std::size_t
    any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
    {
        return impl_->put(buffers, ec);
    }
    void
    any_body::reader::finish(boost::system::error_code& ec)
    {
        impl_->finish(ec);
    }

} // namespace httplib::body
