#include "chunk_writer_impl.hpp"

namespace httplib::client {

chunk_writer::chunk_writer(std::unique_ptr<impl>&& _impl)
    : impl_(std::move(_impl))
{
}

chunk_writer::~chunk_writer() = default;

net::awaitable<void> chunk_writer::write_chunk(std::string_view data)
{
    co_await impl_->write_chunk(data);
}

net::awaitable<void> chunk_writer::close()
{
    co_await impl_->close();
}

} // namespace httplib::client
