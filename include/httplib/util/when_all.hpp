#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

namespace httplib::util {
namespace detail {

template<typename ExecutorType,
         typename ReturnType,
         typename CompletionToken = boost::asio::default_completion_token_t<ExecutorType>>
auto wrap_awaitable(
    const ExecutorType& executor,
    boost::asio::awaitable<ReturnType>&& aw,
    CompletionToken&& token = boost::asio::default_completion_token_t<ExecutorType>())
{
    if constexpr (std::is_void_v<ReturnType>) {
        return boost::asio::async_initiate<CompletionToken, void()>(
            [executor, aw = std::move(aw)](auto&& token) mutable {
                boost::asio::co_spawn(
                    executor,
                    [token = std::move(token),
                     aw    = std::move(aw)]() mutable -> boost::asio::awaitable<void> {
                        co_await std::move(aw);
                        token();
                    },
                    [](std::exception_ptr e) {
                        if (!e)
                            return;

                        std::rethrow_exception(e);
                    });
            },
            token);
    }
    else {
        return boost::asio::async_initiate<CompletionToken, void(ReturnType)>(
            [executor, aw = std::move(aw)](auto&& token) mutable {
                boost::asio::co_spawn(
                    executor,
                    [token = std::move(token),
                     aw    = std::move(aw)]() mutable -> boost::asio::awaitable<void> {
                        auto result = co_await std::move(aw);
                        token(result);
                        co_return;
                    },
                    [](std::exception_ptr e) {
                        if (!e)
                            return;

                        std::rethrow_exception(e);
                    });
            },
            token);
    }
}

} // namespace detail

template<typename ReturnType>
net::awaitable<std::vector<ReturnType>> when_all(std::vector<net::awaitable<ReturnType>>&& ops)
{
    if (ops.empty())
        co_return std::vector<ReturnType> {};

    auto executor = co_await net::this_coro::executor;

    using op_type = decltype(detail::wrap_awaitable(executor, std::move(ops.front())));

    std::vector<op_type> wrap_ops;
    for (auto& v : ops) {
        wrap_ops.push_back(detail::wrap_awaitable(executor, std::move(v)));
    }

    auto [orders, results] = co_await net::experimental::make_parallel_group(std::move(wrap_ops))
                                 .async_wait(net::experimental::wait_for_all(), net::deferred);

    co_return results;
}

static net::awaitable<void> when_all(std::vector<net::awaitable<void>>&& ops)
{
    if (ops.empty())
        co_return;

    auto executor = co_await net::this_coro::executor;

    using op_type = decltype(detail::wrap_awaitable(executor, std::move(ops.front())));

    std::vector<op_type> wrap_ops;
    for (auto& v : ops) {
        wrap_ops.push_back(detail::wrap_awaitable(executor, std::move(v)));
    }

    co_await net::experimental::make_parallel_group(std::move(wrap_ops))
        .async_wait(net::experimental::wait_for_all(), net::deferred);

    co_return;
}

} // namespace httplib::util