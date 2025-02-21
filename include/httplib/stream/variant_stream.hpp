#pragma once
#include "httplib/config.hpp"
#include "httplib/util/type_traits.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <type_traits>
#include <variant>

namespace httplib {

template<typename T>
constexpr inline bool is_basic_stream_v =
    util::is_specialization_v<std::remove_cvref_t<T>, beast::basic_stream>;

template<typename... T>
class variant_stream : public std::variant<T...> {
public:
    using std::variant<T...>::variant;

public:
    using executor_type = net::any_io_executor;
    using lowest_layer_type = tcp::socket::lowest_layer_type;

    executor_type get_executor() {
        return std::visit([&](auto &t) mutable { return t.get_executor(); }, *this);
    }
    lowest_layer_type &lowest_layer() {
        return std::visit(
            [&](auto &t) mutable -> lowest_layer_type & {
                using stream_type = std::decay_t<decltype(beast::get_lowest_layer(t))>;
                if constexpr (is_basic_stream_v<stream_type>) {
                    return beast::get_lowest_layer(t).socket().lowest_layer();
                } else {
                    return t.lowest_layer();
                }
            },
            *this);
    }
    const lowest_layer_type &lowest_layer() const {
        return std::visit(
            [&](auto &t) mutable -> const lowest_layer_type & {
                using stream_type = std::decay_t<decltype(beast::get_lowest_layer(t))>;
                if constexpr (is_basic_stream_v<stream_type>) {
                    return beast::get_lowest_layer(t).socket().lowest_layer();
                } else {
                    return t.lowest_layer();
                }
            },
            *this);
    }
    template<typename MutableBufferSequence, typename ReadHandler>
    auto async_read_some(const MutableBufferSequence &buffers, ReadHandler &&handler) {
        return std::visit(
            [&, handler = std::move(handler)](auto &t) mutable {
                return t.async_read_some(buffers, std::forward<ReadHandler>(handler));
            },
            *this);
    }
    template<typename ConstBufferSequence, typename WriteHandler>
    auto async_write_some(const ConstBufferSequence &buffers, WriteHandler &&handler) {
        return std::visit(
            [&, handler = std::move(handler)](auto &t) mutable {
                return t.async_write_some(buffers, std::forward<WriteHandler>(handler));
            },
            *this);
    }

    tcp::endpoint remote_endpoint() {
        return lowest_layer().remote_endpoint();
    }
    tcp::endpoint remote_endpoint(boost::system::error_code &ec) {
        return lowest_layer().remote_endpoint(ec);
    }

    void shutdown(net::socket_base::shutdown_type what, boost::system::error_code &ec) {
        lowest_layer().shutdown(what, ec);
    }

    bool is_open() const {
        return lowest_layer().is_open();
    }

    void close(boost::system::error_code &ec) {
        lowest_layer().close(ec);
    }
    bool is_connected() {
        if (!lowest_layer().is_open())
            return false;

        boost::system::error_code ec;
        static_cast<tcp::socket&>(lowest_layer()).receive(net::mutable_buffer(), 0, ec);
        return !ec;
    }
};

} // namespace httplib