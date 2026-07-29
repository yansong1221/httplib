#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string_view>

namespace httplib::client {

class chunk_writer
{
public:
    class impl;

    chunk_writer(chunk_writer&&)            = default;
    chunk_writer& operator=(chunk_writer&&) = default;
    ~chunk_writer();

    explicit chunk_writer(std::unique_ptr<impl>&& _impl);

    net::awaitable<void> write_chunk(std::string_view data);
    net::awaitable<void> close();

private:
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::client
