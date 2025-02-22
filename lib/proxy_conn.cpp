#include "proxy_conn.hpp"

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/write.hpp>

namespace httplib
{
namespace detail
{
template<typename S1, typename S2>
net::awaitable<void> transfer(S1& from, S2& to, size_t& bytes_transferred)
{
    bytes_transferred = 0;
    std::vector<uint8_t> buffer;
    buffer.resize(512 * 1024);
    boost::system::error_code ec;

    for (;;)
    {
        auto bytes = co_await from.async_read_some(net::buffer(buffer), net_awaitable[ec]);
        if (ec)
        {
            if (bytes > 0) co_await net::async_write(to, net::buffer(buffer, bytes), net_awaitable[ec]);

            to.shutdown(net::socket_base::shutdown_send, ec);
            co_return;
        }
        co_await net::async_write(to, net::buffer(buffer, bytes), net_awaitable[ec]);
        if (ec)
        {
            to.shutdown(net::socket_base::shutdown_send, ec);
            from.shutdown(net::socket_base::shutdown_receive, ec);
            co_return;
        }
        bytes_transferred += bytes;
    }
}
} // namespace detail

proxy_conn::proxy_conn(http_variant_stream_type&& stream, net::ip::tcp::socket&& proxy_socket)
    : stream_(std::move(stream)), proxy_socket_(std::move(proxy_socket))
{
}
void proxy_conn::read_limit(std::size_t bytes_per_second) { stream_.rate_policy().read_limit(bytes_per_second); }

void proxy_conn::write_limit(std::size_t bytes_per_second) { stream_.rate_policy().write_limit(bytes_per_second); }

httplib::net::awaitable<void> proxy_conn::run()
{
    using namespace net::experimental::awaitable_operators;
    size_t l2r_transferred = 0;
    size_t r2l_transferred = 0;
    co_await (detail::transfer(stream_, proxy_socket_, l2r_transferred) &&
              detail::transfer(proxy_socket_, stream_, r2l_transferred));
}

} // namespace httplib