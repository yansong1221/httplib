#pragma once
#include "httplib/config.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <utility>

namespace httplib::util
{

    template <typename Executor, typename T>
    T
    sync_wait(Executor&& ex, net::awaitable<T>&& aw)
    {
        return net::co_spawn(std::forward<Executor>(ex), std::move(aw), boost::asio::use_future).get();
    }

} // namespace httplib::util
