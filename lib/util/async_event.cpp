#include "httplib/util/async_event.hpp"
#include "async_event_impl.hpp"
#include <utility>

namespace httplib::util
{

    async_event::async_event(net::any_io_executor ex)
        : impl_(std::make_unique<impl>(std::move(ex)))
    {
    }

    async_event::~async_event() = default;

    async_event::notify_result
    async_event::notify()
    {
        return impl_->notify();
    }

    net::awaitable<async_event::wait_result>
    async_event::wait()
    {
        return impl_->wait();
    }

    void
    async_event::close()
    {
        impl_->close();
    }

    bool
    async_event::is_closed() const noexcept
    {
        return impl_->is_closed();
    }

} // namespace httplib::util