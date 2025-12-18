#pragma once

#include <boost/noncopyable.hpp>
#include <boost/pool/object_pool.hpp>

namespace httplib::util {

template<typename T>
class object_pool : public boost::noncopyable
{
public:
    template<typename... Args>
    T* construct(Args&&... args)
    {
        T* ret = nullptr;
        {
            std::lock_guard lck(mutex_);
            ret = pool_.malloc();
        }
        if (ret == 0)
            return ret;
        try {
            new (ret) T(std::forward<Args>(args)...);
        }
        catch (...) {
            std::lock_guard lck(mutex_);
            pool_.free(ret);
            throw;
        }
        return ret;
    }
    void destroy(T* const chunk)
    {
        chunk->~T();
        std::lock_guard lck(mutex_);
        pool_.free(chunk);
    }

    template<typename U = T, typename... Args>
    std::unique_ptr<U, std::function<void(U*)>> make_unique(Args&&... args)
    {
        T* raw = construct(std::forward<Args>(args)...);
        return {raw, [this](U* ptr) { destroy(static_cast<T*>(ptr)); }};
    }

public:
    static object_pool<T>& instance()
    {
        static object_pool<T> g_instance;
        return g_instance;
    }

private:
    boost::object_pool<T> pool_;
    std::mutex mutex_;
};
} // namespace httplib::util