#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/string_hash.hpp"
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httplib::server::middleware
{

    class memory_session_store;

    class HTTPLIB_API session
    {
      public:
        using clock = std::chrono::system_clock;
        using time_point = clock::time_point;

        explicit session(std::string id, time_point created = clock::now());
        session(session const& other);
        session(session&&) noexcept;
        session& operator=(session const& other);
        session& operator=(session&&) noexcept;
        ~session();

        std::string const& id() const;
        time_point created() const;
        time_point last_access() const;
        void touch();

        std::optional<std::string> get(std::string_view key) const;
        void set(std::string key, std::string value);
        bool has(std::string_view key) const;
        void remove(std::string_view key);
        bool empty() const;
        util::string_map<std::string> const& data() const;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;

        /// `memory_session_store` 需要读取 `impl::removed` 才能把副本的删除同步回存量
        /// 会话（区分"显式删除"与"从未存在"）。这是内部簿记，不属于公开 API。
        friend class memory_session_store;
    };

    class HTTPLIB_API session_store
    {
      public:
        virtual ~session_store() = default;
        virtual std::shared_ptr<session> load(std::string_view id) = 0;
        virtual void save(session const& s) = 0;
        virtual void destroy(std::string_view id) = 0;

        /// 可选：限制该实现同时保留的会话数上界，让长期存活的 store 内存有界。
        /// 默认实现忽略该设置；外部自定义 store 不受影响。
        virtual void
        set_max_sessions(std::size_t)
        {
        }
    };

    class HTTPLIB_API session_middleware
    {
      public:
        using value_type = std::shared_ptr<session>;

        session_middleware();
        explicit session_middleware(std::shared_ptr<session_store> store);
        ~session_middleware();

        session_middleware& cookie_name(std::string name);
        session_middleware& cookie_path(std::string path);
        session_middleware& max_age(std::chrono::seconds age);
        session_middleware& http_only(bool v);
        session_middleware& secure(bool v);
        session_middleware& same_site_lax();
        session_middleware& same_site_strict();
        session_middleware& same_site_none();
        session_middleware& store_ttl(std::chrono::seconds ttl);

        /// 会话存储同时保留的会话数上限，默认 8192。仅对实现了该设置的 store
        /// （内置 `memory_session_store`）生效。超出上限时先回收已过期会话，
        /// 仍满则淘汰最久未访问的一条，保证内存有界。
        session_middleware& max_sessions(std::size_t n);

        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

        std::shared_ptr<session_store> store();

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
