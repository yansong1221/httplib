#pragma once
#include "body/buffer_body.hpp"
#include "body/compressed_body.hpp"
#include "body/empty_body.hpp"
#include "body/file_body.hpp"
#include "body/form_data_body.hpp"
#include "body/json_body.hpp"
#include "body/query_params_body.hpp"
#include "body/string_body.hpp"
#include "httplib/config.hpp"
#include <tuple>
#include <type_traits>
#include <variant>

namespace httplib::body
{

    struct any_body
    {
        template <typename... Bodies>
        class variant_value : public std::variant<typename Bodies::value_type...>
        {
          public:
            using std::variant<typename Bodies::value_type...>::variant;

            std::uint64_t decompressed_limit = 0;

            template <typename Body>
            typename Body::value_type&
            as() &
            {
                return std::get<typename Body::value_type>(*this);
            }

            template <typename Body>
            typename Body::value_type const&
            as() const&
            {
                return std::get<typename Body::value_type>(*this);
            }

            template <typename Body>
            bool
            is_body_type() const
            {
                return std::holds_alternative<typename Body::value_type>(*this);
            }
        };

        // 每个 body 都套一层 compressed_body：按 Content-Encoding 透明编解码，
        // 未知/identity 编码时退化为纯转发。value_type 保持不变，故对外 API 不受影响。
        using body_types = std::tuple<compressed_body<empty_body>,
                                      compressed_body<string_body>,
                                      compressed_body<json_body>,
                                      compressed_body<form_data_body>,
                                      compressed_body<file_body>,
                                      compressed_body<query_params_body>,
                                      compressed_body<buffer_body>>;

        template <typename>
        struct value_from_tuple;

        template <typename... Bodies>
        struct value_from_tuple<std::tuple<Bodies...>>
        {
            using value_t = variant_value<Bodies...>;
        };

        using value_type = value_from_tuple<body_types>::value_t;

        class writer;
        class reader;
    };
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

        boost::optional<std::pair<net::const_buffer, bool>>
        get(boost::system::error_code& ec)
        {
            return std::visit(
                [&](auto& w) -> boost::optional<std::pair<net::const_buffer, bool>>
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
        put(net::const_buffer const& buffers, boost::system::error_code& ec)
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
        emplace(http::fields& h, typename Body::value_type& b, std::uint64_t limit = 0)
        {
            auto& r = v.template emplace<typename Body::reader>(h, b);
            r.set_limit(limit);
        }
    };

    class any_body::writer
    {
      public:
        using const_buffers_type = net::const_buffer;

      public:
        template <bool isRequest, class Fields>
        explicit writer(http::header<isRequest, Fields>& h, value_type& b)
            : header_(static_cast<http::fields&>(h))
            , body_(b)
        {
        }
        explicit writer(http::fields& h, value_type& b) : header_(h), body_(b) {}

        void
        init(boost::system::error_code& ec)
        {
            create_writer(header_, body_);
            proxy_.init(ec);
        }

        boost::optional<std::pair<const_buffers_type, bool>>
        get(boost::system::error_code& ec)
        {
            return proxy_.get(ec);
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
        value_type& body_;
        writer_holder proxy_;
    };

    class any_body::reader
    {
      public:
        using const_buffers_type = net::const_buffer;

      public:
        template <bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields>& h, value_type& b)
            : header_(static_cast<http::fields&>(h))
            , body_(b)
        {
        }
        explicit reader(http::fields& h, value_type& b) : header_(h), body_(b) {}

        void
        init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
        {
            auto content_type = header_[http::field::content_type];
            // 必须在 create_reader 之前取：create_reader 可能重设 body_（赋值
            // variant_value 会把 decompressed_limit 一并置 0）。
            auto limit = body_.decompressed_limit;

            if (!std::holds_alternative<body::empty_body::value_type>(body_))
            {
                create_reader(body_, limit);
            }
            else if (content_type.starts_with("multipart/form-data"))
            {
                create_reader<form_data_body>(limit);
            }
            else if (content_type.starts_with("application/json"))
            {
                create_reader<json_body>(limit);
            }
            else if (content_type.starts_with("application/x-www-form-urlencoded"))
            {
                create_reader<query_params_body>(limit);
            }
            else
            {
                create_reader<string_body>(limit);
            }
            proxy_.init(content_length, ec);
        }

        std::size_t
        put(const_buffers_type const& buffers, boost::system::error_code& ec)
        {
            return proxy_.put(buffers, ec);
        }

        void
        finish(boost::system::error_code& ec)
        {
            proxy_.finish(ec);
        }

      private:
        template <typename... Bodies>
        void
        create_reader(any_body::variant_value<Bodies...>& body, std::uint64_t limit)
        {
            std::visit(
                [&](auto& t)
                {
                    using value_type = std::decay_t<decltype(t)>;
                    using body_type = typename detail::match_body<value_type, Bodies...>::type;
                    static_assert(!std::is_void_v<body_type>, "No matching Body type found");

                    proxy_.template emplace<body_type>(header_, t, limit);
                },
                body);
        }

        template <class Body>
        void
        create_reader(std::uint64_t limit)
        {
            if (!std::holds_alternative<typename Body::value_type>(body_))
            {
                body_ = typename Body::value_type {};
            }

            proxy_.template emplace<compressed_body<Body>>(header_, std::get<typename Body::value_type>(body_), limit);
        }

      private:
        http::fields& header_;
        value_type& body_;
        reader_holder proxy_;
    };

} // namespace httplib::body
