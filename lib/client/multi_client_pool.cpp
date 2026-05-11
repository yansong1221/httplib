#include "httplib/client/multi_client_pool.hpp"
namespace httplib::client {
multi_http_client_pool::multi_http_client_pool(const net::any_io_executor& ex,
                                               size_t max_size /*= 10*/)
    : ex_(ex)
    , max_size_(max_size)
{
}
multi_http_client_pool::~multi_http_client_pool()
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, val] : pools_) {
        while (!val.empty()) {
            val.front()->close();
            val.pop();
        }
    }
    pools_.clear();
}
multi_http_client_pool::ClientHandle multi_http_client_pool::acquire(std::string_view host,
                                                                     uint16_t port,
                                                                     bool ssl /*= false*/)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ConnectionInfo info {std::string(host), port, ssl};
    std::unique_ptr<http_client> conn;

    if (pools_.count(info) && !pools_[info].empty()) {
        conn = std::move(pools_[info].front());
        pools_[info].pop();
    }
    else {
        conn = std::make_unique<http_client>(ex_, host, port, ssl);
    }
    return ClientHandle(weak_from_this(), std::move(conn));
}

void multi_http_client_pool::release(std::unique_ptr<http_client> conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ConnectionInfo info {std::string(conn->host()),
                         conn->port()}; // Assuming client has host() and port() methods
    if (pools_[info].size() < max_size_) {
        pools_[info].push(std::move(conn));
    }
}


} // namespace httplib::client