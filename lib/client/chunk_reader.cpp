#include "chunk_reader_impl.hpp"

namespace httplib::client {

chunk_reader::chunk_reader(std::unique_ptr<impl>&& _impl)
    : impl_(std::move(_impl))
{
}

chunk_reader::~chunk_reader() = default;

net::awaitable<std::string_view> chunk_reader::read_chunk()
{
    co_return co_await impl_->read_chunk();
}

bool chunk_reader::is_done() const
{
    return impl_->is_done();
}

} // namespace httplib::client
