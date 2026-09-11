#include "httplib/util/action_queue.hpp"
#include "action_queue_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

namespace httplib::util
{

    action_queue::action_queue(net::any_io_executor const& executor, std::size_t max_pending)
        : impl_(std::make_shared<impl>(executor, max_pending))
    {
    }

    boost::system::error_code
    action_queue::push(act_t&& handler)
    {
        return impl_->push(std::move(handler));
    }

    void
    action_queue::clear()
    {
        impl_->clear();
    }

    std::size_t
    action_queue::pending() const
    {
        return impl_->pending();
    }

    std::shared_future<void>
    action_queue::shutdown()
    {
        return boost::asio::co_spawn(
            impl_->get_executor(),
            [this, self = impl_]() -> net::awaitable<void> { co_return co_await async_shutdown(); },
            boost::asio::use_future);
    }

    httplib::net::awaitable<void>
    action_queue::async_shutdown()
    {
        co_return co_await impl_->async_shutdown();
    }

} // namespace httplib::util