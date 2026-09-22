#pragma once
#include <any>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>

namespace httplib::server
{

    class request_data
    {
      public:
        template <typename T>
        void
        store(T&& val)
        {
            store<T>("", std::forward<T>(val));
        }
        template <typename T>
        void
        store(std::string_view tag, T&& val)
        {
            std::lock_guard lock(mutex_);
            map_[make_key(std::type_index(typeid(std::decay_t<T>)), tag)] = std::any(std::forward<T>(val));
        }
        template <typename T>
        T
        fetch()
        {
            return fetch<T>("");
        }
        template <typename T>
        T
        fetch(std::string_view tag)
        {
            std::lock_guard lock(mutex_);
            return std::any_cast<T>(map_.at(make_key(std::type_index(typeid(T)), tag)));
        }
        template <typename T>
        T
        fetch() const
        {
            return fetch<T>("");
        }
        template <typename T>
        T
        fetch(std::string_view tag) const
        {
            std::lock_guard lock(mutex_);
            return std::any_cast<T const&>(map_.at(make_key(std::type_index(typeid(T)), tag)));
        }
        template <typename T>
        bool
        has() const
        {
            return has<T>("");
        }
        template <typename T>
        bool
        has(std::string_view tag) const
        {
            std::lock_guard lock(mutex_);
            return map_.contains(make_key(std::type_index(typeid(T)), tag));
        }
        template <typename T>
        void
        erase()
        {
            erase<T>("");
        }
        template <typename T>
        void
        erase(std::string_view tag)
        {
            std::lock_guard lock(mutex_);
            map_.erase(make_key(std::type_index(typeid(T)), tag));
        }

      private:
        static std::string
        make_key(std::type_index ti, std::string_view tag)
        {
            std::string k(ti.name());
            if (!tag.empty())
            {
                k += ':';
                k += tag;
            }
            return k;
        }

        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::any> map_;
    };

} // namespace httplib::server
