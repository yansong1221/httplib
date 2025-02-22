#pragma once
#include "httplib/stream/http_stream.hpp"
#include "httplib/use_awaitable.hpp"
#include <memory>

namespace httplib
{
class proxy_conn : public std::enable_shared_from_this<proxy_conn>
{
public:
    explicit proxy_conn(http_variant_stream_type&& stream, net::ip::tcp::socket&& proxy_socket);

public:
    void read_limit(std::size_t bytes_per_second);
    void write_limit(std::size_t bytes_per_second);
    net::awaitable<void> run();

private:
    http_variant_stream_type stream_;
    net::ip::tcp::socket proxy_socket_;
};
} // namespace httplib