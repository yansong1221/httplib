#pragma once
#include <variant>
#ifdef HTTPLIB_ENABLED_SSL
#include "ssl_stream.hpp"
#include <boost/asio/ssl/host_name_verification.hpp>
#endif
#include "boost/asio/use_awaitable.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/system/result.hpp>

namespace httplib
{

    class http_stream
    {
      public:
        using plain_stream = beast::basic_stream<net::ip::tcp, net::any_io_executor, beast::simple_rate_policy>;
#ifdef HTTPLIB_ENABLED_SSL
        using tls_stream = ssl_stream<plain_stream>;
        using stream_t = std::variant<plain_stream, tls_stream>;
#else
        using stream_t = std::variant<plain_stream>;
#endif

      public:
        using executor_type = plain_stream::executor_type;

        executor_type
        get_executor()
        {
            return std::visit([&](auto& t) mutable { return t.get_executor(); }, stream_);
        }
        auto&
        socket()
        {
            return std::visit([&](auto& t) mutable -> auto& { return beast::get_lowest_layer(t).socket(); }, stream_);
        }
        auto const&
        socket() const
        {
            return std::visit([&](auto& t) mutable -> auto const& { return beast::get_lowest_layer(t).socket(); },
                              stream_);
        }
        template <typename MutableBufferSequence, typename ReadHandler>
        auto
        async_read_some(MutableBufferSequence const& buffers, ReadHandler&& handler)
        {
            return std::visit([&, handler = std::forward<ReadHandler>(handler)](auto& t) mutable
                              { return t.async_read_some(buffers, std::forward<ReadHandler>(handler)); },
                              stream_);
        }
        template <typename ConstBufferSequence, typename WriteHandler>
        auto
        async_write_some(ConstBufferSequence const& buffers, WriteHandler&& handler)
        {
            return std::visit([&, handler = std::forward<WriteHandler>(handler)](auto& t) mutable
                              { return t.async_write_some(buffers, std::forward<WriteHandler>(handler)); },
                              stream_);
        }

        bool
        is_open() const
        {
            return socket().is_open();
        }

        void
        expires_after(net::steady_timer::duration const& expiry_time)
        {
            return std::visit([&](auto& t) { beast::get_lowest_layer(t).expires_after(expiry_time); }, stream_);
        }
        void
        expires_at(net::steady_timer::time_point const& expiry_time)
        {
            return std::visit([&](auto& t) { beast::get_lowest_layer(t).expires_at(expiry_time); }, stream_);
        }
        void
        expires_never()
        {
            return std::visit([&](auto& t) { beast::get_lowest_layer(t).expires_never(); }, stream_);
        }

        auto&
        rate_policy() & noexcept
        {
            return std::visit([&](auto& t) mutable -> auto& { return beast::get_lowest_layer(t).rate_policy(); },
                              stream_);
        }
        auto&
        rate_policy() const& noexcept
        {
            return std::visit([&](auto& t) -> auto& { return beast::get_lowest_layer(t).rate_policy(); }, stream_);
        }

        void
        shutdown(net::socket_base::shutdown_type what, boost::system::error_code& ec)
        {
            socket().shutdown(what, ec);
        }
        void
        close()
        {
            std::visit(
                [](auto& stream) mutable
                {
                    using stream_type = std::decay_t<decltype(stream)>;
#ifdef HTTPLIB_ENABLED_SSL
                    if constexpr (std::is_same_v<stream_type, tls_stream>)
                    {
                        boost::system::error_code ec;
                        stream.shutdown(ec);
                    }
#endif
                },
                stream_);
            boost::system::error_code ec;
            socket().shutdown(net::socket_base::shutdown_type::shutdown_both, ec);
            socket().close(ec);
        }

        template <typename EndPoints>
        net::awaitable<boost::system::error_code>
        async_connect(EndPoints&& endpoints)
        {
            boost::system::error_code ec;
            co_await std::visit(
                [&](auto& t) -> net::awaitable<void>
                {
                    using stream_type = std::decay_t<decltype(t)>;
#ifdef HTTPLIB_ENABLED_SSL
                    if constexpr (std::is_same_v<stream_type, http_stream::tls_stream>)
                    {
                        co_await t.next_layer().async_connect(endpoints, util::net_awaitable[ec]);
                        if (ec)
                            co_return;
                        co_await t.async_handshake(ssl::stream_base::client, util::net_awaitable[ec]);
                        co_return;
                    }
#endif
                    if constexpr (std::is_same_v<stream_type, http_stream::plain_stream>)
                    {
                        co_await t.async_connect(endpoints, util::net_awaitable[ec]);
                    }
                },
                stream_);

            if (!ec)
            {
                socket().set_option(net::socket_base::reuse_address(true));
                socket().set_option(net::ip::tcp::no_delay(true));
            }
            co_return ec;
        }

        auto&&
        release()
        {
            return std::move(stream_);
        }

        http_stream(stream_t&& stream) : stream_(std::move(stream)) {}

        static boost::system::result<stream_t>
        create_stream(net::any_io_executor const& executor,
                      std::string const& host,
                      bool use_ssl,
                      bool verify_ssl = true,
                      std::string_view ca_cert = {})
        {
            if (use_ssl)
            {
#ifdef HTTPLIB_ENABLED_SSL
                unsigned long ssl_options
                    = ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::single_dh_use;

                auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
                ssl_ctx->set_options(ssl_options);
                ssl_ctx->set_verify_mode(verify_ssl ? ssl::verify_peer : ssl::verify_none);
                if (verify_ssl)
                {
                    if (!ca_cert.empty())
                    {
                        ssl_ctx->add_certificate_authority(boost::asio::const_buffer(ca_cert.data(), ca_cert.size()));
                    }
                    else
                    {
                        ssl_ctx->set_default_verify_paths();
                    }
                }

                tls_stream stream(executor, ssl_ctx);
                if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
                {
                    beast::error_code ec { static_cast<int>(::ERR_get_error()), net::error::get_ssl_category() };
                    return ec;
                }
                if (verify_ssl)
                {
                    beast::error_code host_ec;
                    stream.set_verify_callback(ssl::host_name_verification(host), host_ec);
                    if (host_ec)
                    {
                        return host_ec;
                    }
                }
                return stream_t(std::move(stream));
#else
                return boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
#endif
            }
            else
            {
                return stream_t(plain_stream(executor));
            }
        }

      private:
        stream_t stream_;
    };

} // namespace httplib