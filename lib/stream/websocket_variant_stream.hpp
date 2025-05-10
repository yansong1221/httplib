#pragma once
#include "variant_stream.hpp"
#include <boost/beast/websocket/stream.hpp>

namespace httplib {

template<typename... T>
class websocket_variant_stream : public http_variant_stream<T...>
{
public:
    using http_variant_stream<T...>::http_variant_stream;

public:
    template<class Header, class AcceptHandler>
    auto async_accept(Header const& req, AcceptHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_accept(req, std::forward<AcceptHandler>(handler));
            },
            *this);
    }
    template<class DynamicBuffer, class ReadHandler>
    auto async_read(DynamicBuffer& buffer, ReadHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_read(buffer, std::forward<ReadHandler>(handler));
            },
            *this);
    }
    template<class ConstBufferSequence, class WriteHandler>
    auto async_write(ConstBufferSequence const& bs, WriteHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_write(bs, std::forward<WriteHandler>(handler));
            },
            *this);
    }
    template<class CloseHandler>
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

} // namespace httplib