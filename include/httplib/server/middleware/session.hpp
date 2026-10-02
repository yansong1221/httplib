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

    /**
     * \brief 一次请求对会话数据所做的改动：键 -> 可选值，\c nullopt 表示显式删除。
     * \details
     * \ref session 与 \ref session_store 之间唯一的写入载体。store 只需原样应用这里
     * 列出的键，不参与「哪些键被改过」的判断 —— 该判断由 \ref session 在本地完成，
     * store 因此不需要触碰 \ref session 的任何内部状态。
     */
    class HTTPLIB_API session_writes
    {
      public:
        /// \brief 记录一次覆盖写。
        /// \param key 键。
        /// \param value 值。
        void set(std::string key, std::string value);

        /// \brief 记录一次删除。
        /// \details
        /// 删除必须能被显式表达：store 只应用补丁里出现的键，无从区分「从未存在」
        /// 与「已被删除」，否则 \ref session::remove 提交后会静默失效。
        /// \param key 待删除的键。
        void remove(std::string_view key);

        bool empty() const;
        std::size_t size() const;

        /// \brief 供 store 实现遍历应用；值为 \c nullopt 的项表示删除。
        util::string_map<std::optional<std::string>> const& entries() const;

      private:
        util::string_map<std::optional<std::string>> entries_;
    };

    class HTTPLIB_API session
    {
      public:
        using clock = std::chrono::system_clock;
        using time_point = clock::time_point;

        explicit session(std::string id, time_point created = clock::now());

        /// \brief 由 store 的存量条目构造快照，供 \ref session_store::load 返回结果。
        /// \details
        /// 新建对象的待提交写入为空，因此它代表「已提交的基线」：本次请求的
        /// \c set / \c remove 才会被记入补丁，提交时不会重复应用上一批改动。
        /// \param id 会话 ID。
        /// \param created 创建时间。
        /// \param last_access 最近访问时间。
        /// \param data 存量数据。
        session(std::string id, time_point created, time_point last_access, util::string_map<std::string> data);

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

        /// \brief 自构造以来累积的写入，供中间件在 \c after 里提交给 store。
        session_writes const& pending_writes() const;

        /// \brief 取出并清空待提交写入。
        /// \details
        /// 提交是唯一的落地点，因此取出后必须已清空，否则同一批写入会被重复应用到
        /// 存量会话上。
        session_writes take_pending_writes();

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    class HTTPLIB_API session_store
    {
      public:
        virtual ~session_store() = default;

        /**
         * \brief 取存量会话的快照；不存在或已过期时返回 \c nullopt。
         * \details
         * 按值返回：调用方拿到的是副本，无法与存量共享可变状态。实现不得返回内部
         * 对象的引用或指针 —— 两个携带同一 session_id 的并发请求会同时拿到它，
         * 随后在各自的 strand 上无锁改同一份数据。
         * \param id 会话 ID。
         * \return 会话快照；不存在或已过期时为空。
         */
        virtual std::optional<session> load(std::string_view id) = 0;

        /**
         * \brief 把 \p writes 原子地应用到 \p id 对应的会话，不存在则创建。
         * \details
         * 契约：实现必须与同一 id 的并发 \c save 原子化，并且只应用 \p writes 里
         * 出现的键。
         * \warning 不得用调用方的快照整体替换存量会话：两个并发请求都基于同一份
         * 基线快照，整体替换会让后到者抹掉先到者的写入。逐键应用即可消除该丢更新。
         * \param id 会话 ID。
         * \param writes 待应用的补丁。
         */
        virtual void save(std::string_view id, session_writes const& writes) = 0;

        /**
         * \brief 删除会话。
         * \details
         * 该删除不是粘性的：store 不记录「已删除」这一事实，只应用补丁里出现的键，
         * 因此任何随后带着补丁的提交都会重建会话。这是「增量提交」契约的直接推论。
         * \param id 会话 ID。
         */
        virtual void destroy(std::string_view id) = 0;

        /// \brief 可选：限制该实现同时保留的会话数上界，让长期存活的 store 内存有界。
        /// \details 默认实现忽略该设置；外部自定义 store 不受影响。
        /// \param max_sessions 会话数上限。
        virtual void
        set_max_sessions(std::size_t max_sessions)
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

        /// \brief 会话存储同时保留的会话数上限，默认 8192。
        /// \details
        /// 仅对实现了 \ref session_store::set_max_sessions 的 store
        /// （内置 \ref memory_session_store）生效。超出上限时先回收已过期会话，
        /// 仍满则淘汰最久未访问的一条，保证内存有界。
        /// \param n 会话数上限。
        session_middleware& max_sessions(std::size_t n);

        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

        std::shared_ptr<session_store> store();

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
