#pragma once
#include "use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <memory>

namespace httplib {

namespace net = boost::asio;
namespace beast = boost::beast;

class proxy_conn : public std::enable_shared_from_this<proxy_conn> {
public:
    explicit proxy_conn(http_stream_variant_type &&stream,
                        net::ip::tcp::socket &&proxy_socket)
        : stream_(std::move(stream)), proxy_socket_(std::move(proxy_socket)) {}

public:
    void read_limit(std::size_t bytes_per_second) {
        stream_.rate_policy().read_limit(bytes_per_second);
    }
    void write_limit(std::size_t bytes_per_second) {
        stream_.rate_policy().write_limit(bytes_per_second);
    }

    net::awaitable<void> run() {
        using namespace net::experimental::awaitable_operators;
        size_t l2r_transferred = 0;
        size_t r2l_transferred = 0;
        co_await (transfer(stream_, proxy_socket_, l2r_transferred) &&
                  transfer(proxy_socket_, stream_, r2l_transferred));
    }

private:
    template<typename S1, typename S2>
    net::awaitable<void> transfer(S1 &from, S2 &to, size_t &bytes_transferred) {
        bytes_transferred = 0;
        std::vector<uint8_t> buffer;
        buffer.resize(512 * 1024);
        boost::system::error_code ec;

        for (;;) {
            auto bytes = co_await from.async_read_some(net::buffer(buffer), net_awaitable[ec]);
            if (ec) {
                if (bytes > 0)
                    co_await net::async_write(to, net::buffer(buffer, bytes), net_awaitable[ec]);

                to.shutdown(net::socket_base::shutdown_send, ec);
                co_return;
            }
            co_await net::async_write(to, net::buffer(buffer, bytes), net_awaitable[ec]);
            if (ec) {
                to.shutdown(net::socket_base::shutdown_send, ec);
                from.shutdown(net::socket_base::shutdown_receive, ec);
                co_return;
            }
            bytes_transferred += bytes;
        }
    }

private:
    http_stream_variant_type stream_;
    net::ip::tcp::socket proxy_socket_;
};
} // namespace httplib