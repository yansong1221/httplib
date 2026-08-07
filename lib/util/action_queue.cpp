#include "httplib/util/action_queue.hpp"
#include "action_queue_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

namespace httplib::util
{

    action_queue::action_queue(net::any_io_executor const& executor) : impl_(std::make_shared<impl>(executor)) {}

    void
    action_queue::push(act_t&& handler)
    {
        impl_->push(std::move(handler));
    }

    void
    action_queue::clear()
    {
        impl_->clear();
    }

    httplib::net::awaitable<void>
    action_queue::async_shutdown()
    {
        co_return co_await impl_->async_shutdown();
    }

    std::shared_future<void>
    action_queue::shutdown()
    {
        return impl_->shutdown();
    }

} // namespace httplib::util
