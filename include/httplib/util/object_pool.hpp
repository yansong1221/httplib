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
        if (!ret)
            throw std::bad_alloc();

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
        if (chunk) {
            chunk->~T();
            std::lock_guard lck(mutex_);
            pool_.free(chunk);
        }
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

template<typename T>
static void pool_unique_ptr_deleter(void* p)
{
    util::object_pool<T>::instance().destroy(static_cast<T*>(p));
}

template<typename T>
using pool_unique_ptr = std::unique_ptr<T, std::function<void(void*)>>;

// template<typename T>
// using pool_unique_ptr = std::unique_ptr<T>;

template<typename T, typename... Args>
inline static pool_unique_ptr<T> make_pool_unique(Args&&... args)
{
    T* p = util::object_pool<T>::instance().construct(std::forward<Args>(args)...);
    return pool_unique_ptr<T>(p, &pool_unique_ptr_deleter<T>);

    /*return std::make_unique<T>(std::forward<Args>(args)...);*/
}

} // namespace httplib::util