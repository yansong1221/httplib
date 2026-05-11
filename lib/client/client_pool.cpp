#include "httplib/client/client_pool.hpp"
namespace httplib::client {

http_client_pool::http_client_pool(const net::any_io_executor& ex,
                                   std::string_view host,
                                   uint16_t port,
                                   bool ssl /*= false*/,
                                   size_t max_size /*= 10*/)
    : ex_(ex)
    , host_(host)
    , port_(port)
    , max_size_(max_size)
    , ssl_(ssl)
{
}
http_client_pool::~http_client_pool()
{
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty()) {
        pool_.front()->close();
        pool_.pop();
    }
}
http_client_pool::ClientHandle http_client_pool::acquire()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<http_client> conn;
    if (!pool_.empty()) {
        conn = std::move(pool_.front());
        pool_.pop();
    }
    else {
        conn = std::make_unique<http_client>(ex_, host_, port_, ssl_);
    }
    return ClientHandle(weak_from_this(), std::move(conn));
}

void http_client_pool::release(std::unique_ptr<http_client> conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool_.size() < max_size_) {
        pool_.push(std::move(conn));
    }
}


} // namespace httplib::client