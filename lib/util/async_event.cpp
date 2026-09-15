#include "httplib/util/async_event.hpp"
#include "async_event_impl.hpp"
#include <utility>

namespace httplib::util
{

    async_event::async_event(net::any_io_executor ex) : impl_(std::make_shared<impl>(std::move(ex))) {}

    async_event::~async_event()
    {
        if (impl_)
        {
            // Closing releases any pending waiters with `closed` instead of
            // leaving them suspended forever. Each in-flight wait holds a
            // shared reference to the implementation, so this never races
            // with a waiter's own state.
            impl_->close();
        }
    }

    net::any_io_executor
    async_event::get_executor() const
    {
        return impl_->get_executor();
    }

    async_event::notify_result
    async_event::notify_one()
    {
        return impl_->notify_one();
    }

    async_event::notify_result
    async_event::notify_all()
    {
        return impl_->notify_all();
    }

    net::awaitable<async_event::wait_result>
    async_event::wait()
    {
        // `impl_` is copied into the coroutine frame, keeping the
        // implementation alive for the whole lifetime of the wait.
        return impl::do_wait(impl_);
    }

    net::awaitable<async_event::wait_result>
    async_event::wait_for(std::chrono::steady_clock::duration timeout)
    {
        return impl::do_wait_for(impl_, timeout);
    }

    bool
    async_event::try_wait()
    {
        return impl_->try_wait();
    }

    void
    async_event::close()
    {
        impl_->close();
    }

    void
    async_event::reset()
    {
        impl_->reset();
    }

    bool
    async_event::is_closed() const noexcept
    {
        return impl_->is_closed();
    }

    bool
    async_event::is_signaled() const
    {
        return impl_->is_signaled();
    }

} // namespace httplib::util
