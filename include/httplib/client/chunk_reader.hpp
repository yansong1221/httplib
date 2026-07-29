#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string_view>

namespace httplib::client {

class HTTPLIB_API chunk_reader
{
public:
    class impl;

    chunk_reader(chunk_reader&&)            = default;
    chunk_reader& operator=(chunk_reader&&) = default;
    ~chunk_reader();

    explicit chunk_reader(std::unique_ptr<impl>&& _impl);

    net::awaitable<std::string_view> read_chunk();
    bool is_done() const;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::client
