#include "httplib/util/action_queue.hpp"
#include "action_queue_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

namespace httplib::util {

action_queue::action_queue(const net::any_io_executor& executor)
    : impl_(std::make_shared<impl>(executor))
{
}


void action_queue::push(act_t&& handler)
{
    impl_->push(std::move(handler));
}

void action_queue::clear()
{
    impl_->clear();
}

httplib::net::awaitable<void> action_queue::async_shutdown(bool cancel_signal /*= true*/)
{
    co_return co_await impl_->async_shutdown(cancel_signal);
}

void action_queue::sync_shutdown(bool cancel_signal /*= true*/)
{
    boost::asio::co_spawn(
        impl_->get_executor(), async_shutdown(cancel_signal), boost::asio::use_future)
        .get();
}

} // namespace httplib::util
