#pragma once
#include "http_stream.hpp"

namespace httplib::stream {

template<typename... T>
class websocket_stream_variant : public http_stream_variant<T...> {
public:
    using http_stream_variant<T...>::http_stream_variant;

public:
    template<class Header, class AcceptHandler>
    auto async_accept(Header const &req, AcceptHandler &&handler) {
        return std::visit(
            [&, handler = std::move(handler)](auto &t) mutable {
                return t.async_accept(req, std::forward<AcceptHandler>(handler));
            },
            *this);
    }
    template<class DynamicBuffer, class ReadHandler>
    auto async_read(DynamicBuffer &buffer, ReadHandler &&handler) {
        return std::visit(
            [&, handler = std::move(handler)](auto &t) mutable {
                return t.async_read(buffer, std::forward<ReadHandler>(handler));
            },
            *this);
    }
    template<class ConstBufferSequence, class WriteHandler>
    auto async_write(ConstBufferSequence const &bs, WriteHandler &&handler) {
        return std::visit(
            [&, handler = std::move(handler)](auto &t) mutable {
                return t.async_write(bs, std::forward<WriteHandler>(handler));
            },
            *this);
    }
    template<class CloseHandler>
    auto async_close(beast::websocket::close_reason const &cr, CloseHandler &&handler) {
        return std::visit(
            [&, handler = std::move(handler)](auto &t) mutable {
                return t.async_close(cr, std::forward<CloseHandler>(handler));
            },
            *this);
    }
    bool got_binary() const noexcept {
        return std::visit([&](auto &t) mutable { return t.got_binary(); }, *this);
    }
    bool got_text() const {
        return std::visit([&](auto &t) mutable { return t.got_text(); }, *this);
    }
    void text(bool value) {
        std::visit([&](auto &t) mutable { return t.text(value); }, *this);
    }
    void binary(bool value) {
        std::visit([&](auto &t) mutable { return t.binary(value); }, *this);
    }
};

using ws_stream = websocket::stream<http_stream>;
using ssl_ws_stream = websocket::stream<ssl_http_stream>;

using ws_stream_variant_type = websocket_stream_variant<ws_stream, ssl_ws_stream>;

} // namespace httplib::stream