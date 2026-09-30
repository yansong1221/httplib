#pragma once
#include "httplib/util/string_hash.hpp"
#include <any>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
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
            map_[make_key<T>(tag)] = std::any(std::forward<T>(val));
        }
        /// \brief 取值；键不存在时返回空 optional，不抛异常。
        ///
        /// 返回 `decay_t<T>`：键里存的就是去修饰后的类型（见 make_key），所以请求
        /// `const int` / `int&` 也能命中同一个键，返回的仍是那个值类型副本。
        ///
        /// 取值一律**拷贝**，不会把存储里的值搬走，因此对 const / 非 const 的
        /// request_data 表现一致。单次调用内只加一次锁，消除了「先 has() 判断、再另行取值」
        /// 这种两次加锁之间的竞态窗口——这正是本方法存在的理由。
        template <typename T>
        std::optional<std::decay_t<T>>
        fetch() const
        {
            return fetch<T>("");
        }
        template <typename T>
        std::optional<std::decay_t<T>>
        fetch(std::string_view tag) const
        {
            std::lock_guard lock(mutex_);
            auto it = map_.find(make_key<T>(tag));
            if (it == map_.end())
            {
                return std::nullopt;
            }
            // 这里 any_cast 不会抛：键由 typeid(decay_t<T>) 构成，键命中即说明存的
            // 正是 decay_t<T>，而 any_cast 参数用的也是同一个 decay 后的类型。
            return std::any_cast<std::decay_t<T>>(it->second);
        }
        /// \brief 判断该类型 / tag 是否已存入；不取值，因此不会拷贝。
        ///
        /// 仅适合纯谓词用途。**不要**用 has() 判断后再另行取值——那是两次加锁，
        /// 中间可被并发 erase() 打断；需要取值时请直接用 fetch()。
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
            return map_.contains(make_key<T>(tag));
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
            map_.erase(make_key<T>(tag));
        }

      private:
        /// 键 = typeid(decay_t<T>).name() [":" + tag]。
        ///
        /// 用 decay_t 是为了让存取两侧共用同一套规则：存进去时 T 是按值构造进 std::any
        /// 的（引用与 cv 限定在存入时就已抹掉），所以查的时候也必须先抹掉，否则
        /// `store(x)` 存出的键是 `int`，而 `fetch<const int>()` 查的是 `const int`，
        /// 两者是不同键，会静默 miss。
        ///
        /// 类型本身是键的一部分，因此键命中即意味着存的正是 decay_t<T>——这也是
        /// fetch 里 any_cast 不可能失败、无需区分「键不存在」和「类型不符」的依据。
        template <typename T>
        static std::string
        make_key(std::string_view tag)
        {
            using type = std::decay_t<T>;
            std::string k(typeid(type).name());
            if (!tag.empty())
            {
                k += ':';
                k += tag;
            }
            return k;
        }

        mutable std::mutex mutex_;
        util::string_map<std::any> map_;
    };

} // namespace httplib::server
