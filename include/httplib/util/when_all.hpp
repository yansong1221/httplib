#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace httplib::util
{
    namespace detail
    {

        template <typename T>
        struct awaitable_result;

        template <typename R>
        struct awaitable_result<net::awaitable<R>>
        {
            using type = R;
        };

        template <typename T>
        using awaitable_result_t = typename awaitable_result<T>::type;

        template <typename ExecutorType,
                  typename F,
                  typename CompletionToken = boost::asio::default_completion_token_t<ExecutorType>>
        auto
        wrap_awaitable(ExecutorType const& executor,
                       F&& f,
                       CompletionToken&& token = boost::asio::default_completion_token_t<ExecutorType>())
        {
            return boost::asio::async_initiate<CompletionToken, void(std::exception_ptr)>(
                [executor, f = std::move(f)](auto&& completion_handler) mutable
                {
                    boost::asio::co_spawn(
                        executor,
                        [completion_handler = std::move(completion_handler),
                         f = std::move(f)]() mutable -> boost::asio::awaitable<void>
                        {
                            std::exception_ptr eptr;
                            try
                            {
                                co_await f();
                            }
                            catch (...)
                            {
                                eptr = std::current_exception();
                            }
                            completion_handler(std::move(eptr));
                        },
                        [](std::exception_ptr e)
                        {
                            if (e)
                            {
                                std::rethrow_exception(e);
                            }
                        });
                },
                token);
        }

        template <typename ReturnType,
                  typename ExecutorType,
                  typename F,
                  typename CompletionToken = boost::asio::default_completion_token_t<ExecutorType>>
        auto
        wrap_awaitable(ExecutorType const& executor,
                       F&& f,
                       CompletionToken&& token = boost::asio::default_completion_token_t<ExecutorType>())
        {
            return boost::asio::async_initiate<CompletionToken, void(std::exception_ptr, ReturnType)>(
                [executor, f = std::move(f)](auto&& completion_handler) mutable
                {
                    boost::asio::co_spawn(
                        executor,
                        [completion_handler = std::move(completion_handler),
                         f = std::move(f)]() mutable -> boost::asio::awaitable<void>
                        {
                            std::exception_ptr eptr;
                            ReturnType result {};
                            try
                            {
                                result = co_await f();
                            }
                            catch (...)
                            {
                                eptr = std::current_exception();
                            }
                            completion_handler(std::move(eptr), std::move(result));
                        },
                        [](std::exception_ptr e)
                        {
                            if (e)
                            {
                                std::rethrow_exception(e);
                            }
                        });
                },
                token);
        }

    } // namespace detail

    template <typename ReturnType>
    net::awaitable<std::vector<ReturnType>>
    when_all(std::vector<std::function<net::awaitable<ReturnType>()>>&& ops)
    {
        if (ops.empty())
        {
            co_return std::vector<ReturnType> {};
        }

        auto executor = co_await net::this_coro::executor;

        using op_type = decltype(detail::wrap_awaitable<ReturnType>(executor, std::move(ops.front()), net::deferred));

        std::vector<op_type> wrap_ops;
        wrap_ops.reserve(ops.size());
        for (auto& op : ops)
        {
            wrap_ops.push_back(detail::wrap_awaitable<ReturnType>(executor, std::move(op), net::deferred));
        }

        auto [orders, eptrs, values] = co_await net::experimental::make_parallel_group(std::move(wrap_ops))
                                           .async_wait(net::experimental::wait_for_all(), net::deferred);

        // `eptrs` and `values` are already indexed by operation index (i.e. the
        // order the operations were added), matching `ops`. `orders` records the
        // completion order and is intentionally not used for result mapping.
        std::vector<ReturnType> out;
        out.reserve(values.size());
        std::exception_ptr first_error;

        for (size_t i = 0; i < values.size(); ++i)
        {
            if (eptrs[i] && !first_error)
            {
                first_error = eptrs[i];
            }
            out.push_back(std::move(values[i]));
        }

        if (first_error)
        {
            std::rethrow_exception(first_error);
        }

        co_return out;
    }
    template <typename ReturnType>
    net::awaitable<std::vector<ReturnType>>
    when_all(std::vector<net::awaitable<ReturnType>>&& ops)
    {
        if (ops.empty())
        {
            co_return std::vector<ReturnType> {};
        }

        std::vector<std::function<net::awaitable<ReturnType>()>> new_ops;
        for (auto&& op : ops)
        {
            auto op_ptr = std::make_shared<std::decay_t<decltype(op)>>(std::move(op));
            new_ops.emplace_back([op_ptr]() -> net::awaitable<ReturnType> { co_return co_await std::move(*op_ptr); });
        }
        co_return co_await when_all(std::move(new_ops));
    }

    static net::awaitable<void>
    when_all(std::vector<std::function<net::awaitable<void>()>>&& ops)
    {
        if (ops.empty())
        {
            co_return;
        }

        auto executor = co_await net::this_coro::executor;

        using op_type = decltype(detail::wrap_awaitable(executor, std::move(ops.front()), net::deferred));

        std::vector<op_type> wrap_ops;
        wrap_ops.reserve(ops.size());
        for (auto& op : ops)
        {
            wrap_ops.push_back(detail::wrap_awaitable(executor, std::move(op), net::deferred));
        }

        auto [orders, results] = co_await net::experimental::make_parallel_group(std::move(wrap_ops))
                                     .async_wait(net::experimental::wait_for_all(), net::deferred);

        std::exception_ptr first_error;

        for (auto& eptr : results)
        {
            if (eptr && !first_error)
            {
                first_error = eptr;
            }
        }

        if (first_error)
        {
            std::rethrow_exception(first_error);
        }

        co_return;
    }
    static net::awaitable<void>
    when_all(std::vector<net::awaitable<void>>&& ops)
    {
        if (ops.empty())
        {
            co_return;
        }
        std::vector<std::function<net::awaitable<void>()>> new_ops;
        for (auto&& op : ops)
        {
            auto op_ptr = std::make_shared<std::decay_t<decltype(op)>>(std::move(op));
            new_ops.emplace_back(
                [op_ptr]() -> net::awaitable<void>
                {
                    co_await std::move(*op_ptr);
                    co_return;
                });
        }
        co_await when_all(std::move(new_ops));
        co_return;
    }

    template <typename F, typename... Fs, typename ReturnType = detail::awaitable_result_t<std::invoke_result_t<F>>>
    net::awaitable<std::vector<ReturnType>>
    when_all(F&& f, Fs&&... fs)
    {
        std::vector<std::function<net::awaitable<ReturnType>()>> ops;
        ops.reserve(1 + sizeof...(Fs));
        ops.push_back(std::forward<F>(f));
        (ops.push_back(std::forward<Fs>(fs)), ...);

        co_return co_await when_all(std::move(ops));
    }

} // namespace httplib::util
