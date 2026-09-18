#include "httplib/util/async_mutex.hpp"
#include "async_mutex_impl.hpp"
#include <utility>

namespace httplib::util
{

    async_mutex::async_mutex(net::any_io_executor ex) : impl_(std::make_shared<impl>(std::move(ex))) {}

    async_mutex::~async_mutex()
    {
        // close() 只改变 impl 状态；waiter / guard 仍通过 shared_ptr 持有 impl。
        if (impl_)
        {
            impl_->close();
        }
    }

    net::any_io_executor
    async_mutex::get_executor() const noexcept
    {
        return impl_->get_executor();
    }

    net::awaitable<async_mutex::guard>
    async_mutex::lock()
    {
        // 拷贝 impl_ 到协程 frame，保证等待期间即使 async_mutex 析构，
        // 内部状态仍然存活。
        auto self = impl_;

        co_return co_await self->async_lock();
    }

    net::awaitable<async_mutex::guard>
    async_mutex::lock_for(duration timeout)
    {
        auto self = impl_;

        co_return co_await self->async_lock_for(timeout);
    }

    async_mutex::guard
    async_mutex::try_lock()
    {
        return impl_->try_lock();
    }

    bool
    async_mutex::is_locked() const noexcept
    {
        return impl_->is_locked();
    }

    bool
    async_mutex::is_closed() const noexcept
    {
        return impl_->is_closed();
    }

    std::size_t
    async_mutex::waiter_count() const noexcept
    {
        return impl_->waiter_count();
    }

    void
    async_mutex::close() noexcept
    {
        impl_->close();
    }

    /*
     * ------------------------------------------------------------
     * async_mutex::guard
     * ------------------------------------------------------------
     */

    async_mutex::guard::guard() noexcept = default;

    async_mutex::guard::~guard() noexcept { reset(); }

    async_mutex::guard::guard(guard&& other) noexcept
        : impl_(std::move(other.impl_))
        , token_(other.token_)
        , status_(other.status_)
        , active_(other.active_)
    {
        other.token_ = 0;
        other.status_ = lock_status::failed;
        other.active_ = false;
    }

    async_mutex::guard&
    async_mutex::guard::operator=(guard&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        reset();

        impl_ = std::move(other.impl_);
        token_ = other.token_;
        status_ = other.status_;
        active_ = other.active_;

        other.token_ = 0;
        other.status_ = lock_status::failed;
        other.active_ = false;

        return *this;
    }

    bool
    async_mutex::guard::owns_lock() const noexcept
    {
        return active_ && status_ == lock_status::acquired && token_ != 0 && impl_ != nullptr;
    }

    async_mutex::guard::operator bool() const noexcept { return owns_lock(); }

    lock_status
    async_mutex::guard::status() const noexcept
    {
        return status_;
    }

    void
    async_mutex::guard::reset() noexcept
    {
        if (!active_)
        {
            return;
        }

        active_ = false;

        auto impl = std::move(impl_);
        auto const token = std::exchange(token_, 0);

        if (!impl || token == 0)
        {
            return;
        }

        // 不能从 RAII destructor 向外传播异常：unlock() 内部全部是
        // mutex lock / state transition / event notify。
        impl->unlock(token);
    }

    void
    async_mutex::guard::unlock() noexcept
    {
        reset();
    }

    async_mutex::guard::guard(std::shared_ptr<impl> impl, std::uint64_t token) noexcept
        : impl_(std::move(impl))
        , token_(token)
        , status_(lock_status::acquired)
        , active_(true)
    {
    }

    async_mutex::guard::guard(lock_status status) noexcept : status_(status) {}

} // namespace httplib::util
