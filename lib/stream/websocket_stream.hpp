#pragma once
#ifdef HTTPLIB_ENABLED_SSL
#include "ssl_stream.hpp"
#endif
#include "http_stream.hpp"
#include <boost/beast/websocket/stream.hpp>

namespace httplib {

class websocket_stream
{
public:
    using plain_stream = websocket::stream<http_stream::plain_stream>;
#ifdef HTTPLIB_ENABLED_SSL
    using tls_stream = websocket::stream<http_stream::tls_stream>;
    using stream_t   = std::variant<plain_stream, tls_stream>;
#else
    using stream_t = std::variant<plain_stream>;
#endif
    static std::unique_ptr<websocket_stream> create(http_stream&& stream)
    {
        return std::make_unique<websocket_stream>(std::move(stream));
    }

public:
    bool is_open() const
    {
        return std::visit([](auto& t) mutable { return t.is_open(); }, stream_);
    }
    auto& socket() &
    {
        return std::visit(
            [](auto& t) mutable -> auto& { return beast::get_lowest_layer(t).socket(); }, stream_);
    }
    const auto& socket() const& noexcept
    {
        return std::visit(
            [&](auto& t) -> const auto& { return beast::get_lowest_layer(t).socket(); }, stream_);
    }

    template<class Header, class AcceptHandler>
    auto async_accept(Header const& req, AcceptHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_accept(req, std::forward<AcceptHandler>(handler));
            },
            stream_);
    }
    template<class HandshakeHandler>
    auto async_handshake(std::string_view host, std::string_view target, HandshakeHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_handshake(host, target, std::forward<HandshakeHandler>(handler));
            },
            stream_);
    }
    void set_option(auto&& opt)
    {
        std::visit([opt = std::forward<decltype(opt)>(opt)](
                       auto& s) mutable { s.set_option(std::forward<decltype(opt)>(opt)); },
                   stream_);
    }

    template<class DynamicBuffer, class ReadHandler>
    auto async_read(DynamicBuffer& buffer, ReadHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_read(buffer, std::forward<ReadHandler>(handler));
            },
            stream_);
    }
    template<class ConstBufferSequence, class WriteHandler>
    auto async_write(ConstBufferSequence const& bs, WriteHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_write(bs, std::forward<WriteHandler>(handler));
            },
            stream_);
    }
    template<class CloseHandler>
    auto async_close(beast::websocket::close_reason const& cr, CloseHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_close(cr, std::forward<CloseHandler>(handler));
            },
            stream_);
    }
    template<class PingHandler>
    auto async_ping(beast::websocket::ping_data const& payload, PingHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_ping(payload, std::forward<PingHandler>(handler));
            },
            stream_);
    }
    bool got_binary() const noexcept
    {
        return std::visit([&](auto& t) mutable { return t.got_binary(); }, stream_);
    }
    bool got_text() const noexcept
    {
        return std::visit([&](auto& t) mutable { return t.got_text(); }, stream_);
    }
    void text(bool value)
    {
        std::visit([&](auto& t) mutable { return t.text(value); }, stream_);
    }
    void binary(bool value)
    {
        std::visit([&](auto& t) mutable { return t.binary(value); }, stream_);
    }

    websocket_stream(stream_t&& stream)
        : stream_(std::move(stream))
    {
    }

    websocket_stream(http_stream&& stream)
        : stream_(std::visit(
              [](auto&& t) {
                  using value_type = std::decay_t<decltype(t)>;

                  if constexpr (std::same_as<http_stream::plain_stream, value_type>) {
                      return stream_t(plain_stream(std::move(t)));
                  }
#ifdef HTTPLIB_ENABLED_SSL
                  else if constexpr (std::same_as<http_stream::tls_stream, value_type>) {
                      return stream_t(tls_stream(std::move(t)));
                  }
#endif
              },
              stream.release()))
    {
    }

private:
    stream_t stream_;
};

} // namespace httplib