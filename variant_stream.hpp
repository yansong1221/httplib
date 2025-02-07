#pragma once
#include <type_traits>
#include <variant>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace util {

namespace net       = boost::asio;
using tcp           = net::ip::tcp;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;

template <typename... T>
class variant_stream : public std::variant<T...> {
public:
    variant_stream()                            = default;
    variant_stream& operator=(variant_stream&&) = default;
    variant_stream(variant_stream&&)            = default;

    template <typename S>
    explicit variant_stream(S device)
        : std::variant<T...>(std::move(device))
    {
        static_assert(std::is_move_constructible<S>::value, "must be move constructible");
    }

public:
    using executor_type     = net::any_io_executor;
    using lowest_layer_type = tcp::socket::lowest_layer_type;

    executor_type get_executor()
    {
        return std::visit([&](auto& t) mutable { return t.get_executor(); }, *this);
    }
    lowest_layer_type& lowest_layer()
    {
        return std::visit(
            [&](auto& t) mutable -> lowest_layer_type& {
                using stream_type = std::decay_t<decltype(beast::get_lowest_layer(t))>;
                if constexpr (std::same_as<stream_type, beast::tcp_stream>) {
                    return beast::get_lowest_layer(t).socket().lowest_layer();
                }
                else {
                    return t.lowest_layer();
                }
            },
            *this);
    }
    const lowest_layer_type& lowest_layer() const
    {
        return std::visit(
            [&](auto& t) mutable -> const lowest_layer_type& {
                using stream_type = std::decay_t<decltype(beast::get_lowest_layer(t))>;
                if constexpr (std::same_as<stream_type, beast::tcp_stream>) {
                    return beast::get_lowest_layer(t).socket().lowest_layer();
                }
                else {
                    return t.lowest_layer();
                }
            },
            *this);
    }
    template <typename MutableBufferSequence, typename ReadHandler>
    auto async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_read_some(buffers, std::forward<ReadHandler>(handler));
            },
            *this);
    }
    template <typename ConstBufferSequence, typename WriteHandler>
    auto async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_write_some(buffers, std::forward<WriteHandler>(handler));
            },
            *this);
    }

    tcp::endpoint remote_endpoint()
    {
        return lowest_layer().remote_endpoint();
    }

    void shutdown(net::socket_base::shutdown_type what, boost::system::error_code& ec)
    {
        lowest_layer().shutdown(what, ec);
    }

    bool is_open() const
    {
        return lowest_layer().is_open();
    }

    void close(boost::system::error_code& ec)
    {
        lowest_layer().close(ec);
    }
};

template <typename... T>
class http_variant_stream : public variant_stream<T...> {
    using variant_stream<T...>::variant_stream;

public:
    auto expires_after(const net::steady_timer::duration& expiry_time)
    {
        return std::visit(
            [&](auto& t) mutable { return beast::get_lowest_layer(t).expires_after(expiry_time); },
            *this);
    }
    auto expires_never()
    {
        return std::visit([&](auto& t) mutable { return beast::get_lowest_layer(t).expires_never(); },
                          *this);
    }
};

template <typename... T>
class websocket_variant_stream : public http_variant_stream<T...> {
public:
    using http_variant_stream<T...>::http_variant_stream;

public:
    template <class Header, class AcceptHandler>
    auto async_accept(Header const& req, AcceptHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_accept(req, std::forward<AcceptHandler>(handler));
            },
            *this);
    }
    template <class DynamicBuffer, class ReadHandler>
    auto async_read(DynamicBuffer& buffer, ReadHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_read(buffer, std::forward<ReadHandler>(handler));
            },
            *this);
    }
    template <class ConstBufferSequence, class WriteHandler>
    auto async_write(ConstBufferSequence const& bs, WriteHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_write(bs, std::forward<WriteHandler>(handler));
            },
            *this);
    }
    template <class CloseHandler>
    auto async_close(beast::websocket::close_reason const& cr, CloseHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_close(cr, std::forward<CloseHandler>(handler));
            },
            *this);
    }
    bool got_binary() const noexcept
    {
        return std::visit([&](auto& t) mutable { return t.got_binary(); }, *this);
    }
    bool got_text() const
    {
        return std::visit([&](auto& t) mutable { return t.got_text(); }, *this);
    }
    void text(bool value)
    {
        std::visit([&](auto& t) mutable { return t.text(value); }, *this);
    }
    void binary(bool value)
    {
        std::visit([&](auto& t) mutable { return t.binary(value); }, *this);
    }
};

using http_stream              = beast::tcp_stream;
using ssl_http_stream          = beast::ssl_stream<beast::tcp_stream>;
using http_variant_stream_type = http_variant_stream<http_stream, ssl_http_stream>;

using ws_stream              = websocket::stream<http_stream>;
using ssl_ws_stream          = websocket::stream<ssl_http_stream>;
using ws_variant_stream_type = websocket_variant_stream<ws_stream, ssl_ws_stream>;

}  // namespace util