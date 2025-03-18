#pragma once
#include "httplib/config.hpp"
#include "stream/http_stream.hpp"
#include <memory>

namespace httplib {
class server;
class session : public std::enable_shared_from_this<session> {
public:
    explicit session(tcp::socket&& sock);

public:
    void close();

private:
    http_stream stream_;
};
} // namespace httplib