#pragma once

#include <boost/pool/object_pool.hpp>
#include <boost/serialization/singleton.hpp>

namespace httplib::util {

template<typename T>
class object_pool : public boost::serialization::singleton<object_pool<T>>
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

    template<typename... Args>
    std::unique_ptr<T, std::function<void(T*)>> make_unique(Args&&... args)
    {
        T* raw = construct(std::forward<Args>(args)...);
        return {raw, [this](T* ptr) { destroy(ptr); }};
    }

private:
    boost::object_pool<T> pool_;
    std::mutex mutex_;
};
} // namespace httplib::util