#include "httplib/util/action_queue.hpp"
#include "action_queue_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <utility>

namespace httplib::util
{

    action_queue::action_queue(net::any_io_executor const& executor, std::size_t max_pending, error_handler_t on_error)
        : impl_(impl::create(executor, max_pending, std::move(on_error)))
    {
    }

    action_queue::~action_queue()
    {
        if (impl_)
        {
            // An idle worker holds a shared reference to impl, so without this
            // the worker (and impl) would outlive us. cancel() wakes it and lets
            // it observe the stop request and exit.
            impl_->cancel();
        }
    }

    boost::system::error_code
    action_queue::push(act_t handler)
    {
        return impl_->push(std::move(handler));
    }

    void
    action_queue::clear()
    {
        impl_->clear();
    }

    void
    action_queue::cancel()
    {
        impl_->cancel();
    }

    std::size_t
    action_queue::pending() const
    {
        return impl_->pending();
    }

    net::awaitable<void>
    action_queue::async_shutdown()
    {
        // Keep impl alive across the suspension even if the owner is destroyed.
        auto self = impl_;
        co_return co_await self->async_shutdown();
    }

    std::future<void>
    action_queue::shutdown()
    {
        auto self = impl_;
        return boost::asio::co_spawn(
            self->get_executor(),
            [self]() -> net::awaitable<void> { co_return co_await self->async_shutdown(); },
            boost::asio::use_future);
    }

} // namespace httplib::util
