#include "beast_alias.hpp"
#include "html/cookie.hpp"
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/util/string_hash.hpp"
#include "memory_store.hpp"
#include <atomic>
#include <mutex>
#include <random>
#include <utility>

namespace httplib::server::middleware
{

    namespace
    {

        struct session_config
        {
            std::string cookie_name = "session_id";
            std::string cookie_path = "/";
            std::chrono::seconds max_age { 0 };
            bool http_only = true;
            bool secure = false;
            httplib::html::cookie::same_site_t same_site = httplib::html::cookie::same_site_t::lax;
            std::chrono::seconds store_ttl = std::chrono::hours(24);
        };

    } // namespace

    // ---- session_writes ----

    void
    session_writes::set(std::string key, std::string value)
    {
        entries_[std::move(key)] = std::optional<std::string>(std::move(value));
    }

    void
    session_writes::remove(std::string_view key)
    {
        entries_[std::string(key)] = std::nullopt;
    }

    bool
    session_writes::empty() const
    {
        return entries_.empty();
    }

    std::size_t
    session_writes::size() const
    {
        return entries_.size();
    }

    util::string_map<std::optional<std::string>> const&
    session_writes::entries() const
    {
        return entries_;
    }

    // ---- session ----

    struct session::impl
    {
        std::string id;
        session::time_point created;
        session::time_point last_access;
        util::string_map<std::string> data;

        /// 本对象上尚未提交到 store 的写入，即这次请求对会话做过的改动。
        ///
        /// 工作副本来自 `load()` 的基线快照，期间的写入都落在这里。提交时只把这些
        /// 键交给 store：若把整张 data 交出去，基线里本次没动过的键也会被写回，
        /// 从而覆盖并发请求对这些键的修改（lost update）。
        session_writes pending;
    };

    session::session(std::string id, time_point created)
        : impl_(std::make_unique<impl>(std::move(id), created, created))
    {
    }

    session::session(std::string id, time_point created, time_point last_access, util::string_map<std::string> data)
        : impl_(std::make_unique<impl>(std::move(id), created, last_access, std::move(data)))
    {
    }

    session::session(session const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}
    session::session(session&&) noexcept = default;
    session::~session() = default;
    session&
    session::operator=(session const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    session& session::operator=(session&&) noexcept = default;

    std::string const&
    session::id() const
    {
        return impl_->id;
    }
    session::time_point
    session::created() const
    {
        return impl_->created;
    }
    session::time_point
    session::last_access() const
    {
        return impl_->last_access;
    }
    void
    session::touch()
    {
        impl_->last_access = clock::now();
    }

    std::optional<std::string>
    session::get(std::string_view key) const
    {
        auto it = impl_->data.find(key);
        if (it != impl_->data.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void
    session::set(std::string key, std::string value)
    {
        impl_->data.insert_or_assign(key, value);
        impl_->pending.set(std::move(key), std::move(value));
    }

    bool
    session::has(std::string_view key) const
    {
        return impl_->data.count(key) > 0;
    }

    void
    session::remove(std::string_view key)
    {
        impl_->data.erase(std::string(key));
        impl_->pending.remove(key);
    }

    bool
    session::empty() const
    {
        return impl_->data.empty();
    }

    util::string_map<std::string> const&
    session::data() const
    {
        return impl_->data;
    }

    session_writes const&
    session::pending_writes() const
    {
        return impl_->pending;
    }

    session_writes
    session::take_pending_writes()
    {
        return std::exchange(impl_->pending, session_writes {});
    }

    // ---- memory_session_store ----

    class memory_session_store::impl
    {
      public:
        using clock = session::clock;
        using time_point = session::time_point;

        /// 存量会话。
        ///
        /// 刻意用裸结构而非 `session`：store 不参与「哪些键被改过」的判断，因此既不
        /// 需要 `session` 的增量簿记，也不需要它的私有访问权。
        struct entry
        {
            std::string id;
            time_point created;
            time_point last_access;
            util::string_map<std::string> data;
        };

        std::chrono::seconds ttl_;
        std::mutex mutex_;
        util::string_map<entry> sessions_;

        std::size_t max_sessions_ = 8192;
        time_point last_sweep_ {};

        bool
        is_expired(entry const& e, time_point now) const
        {
            return (now - e.last_access) > ttl_;
        }

        /// 回收扫描的节流间隔：TTL 的四分之一，下限 1s。
        /// 既保证过期条目不会长期滞留，又让单次请求的额外成本保持均摊。
        std::chrono::seconds
        sweep_interval() const
        {
            auto quarter = ttl_ / 4;
            return quarter < std::chrono::seconds(1) ? std::chrono::seconds(1) : quarter;
        }

        /// 调用方必须持有 mutex_。
        void
        sweep(time_point now)
        {
            if (last_sweep_ != time_point {} && now - last_sweep_ < sweep_interval())
            {
                return;
            }
            last_sweep_ = now;
            erase_expired(now);
        }

        /// 调用方必须持有 mutex_。
        void
        erase_expired(time_point now)
        {
            for (auto it = sessions_.begin(); it != sessions_.end();)
            {
                if (is_expired(it->second, now))
                {
                    it = sessions_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        /// 调用方必须持有 mutex_。淘汰最久未访问的一条，保证插入后不越界。
        void
        evict_oldest()
        {
            auto victim = sessions_.end();
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
            {
                if (victim == sessions_.end() || it->second.last_access < victim->second.last_access)
                {
                    victim = it;
                }
            }
            if (victim != sessions_.end())
            {
                sessions_.erase(victim);
            }
        }

        /// 调用方必须持有 mutex_。只应用补丁里出现的键：存量里没被本次补丁提到的键
        /// 一律不碰，这样并发请求各自提交的写入才不会互相覆盖。
        void
        apply(entry& target, session_writes const& writes)
        {
            for (auto const& [key, value] : writes.entries())
            {
                if (value)
                {
                    target.data.insert_or_assign(key, *value);
                }
                else
                {
                    // 本次请求显式删除过：即便存量里存在（可能是并发请求刚写的），也要删掉。
                    target.data.erase(key);
                }
            }
        }
    };

    memory_session_store::memory_session_store(std::chrono::seconds ttl)
    {
        auto p = new impl {};
        p->ttl_ = ttl;
        impl_.reset(p);
    }

    memory_session_store::~memory_session_store() = default;

    std::optional<session>
    memory_session_store::load(std::string_view id)
    {
        std::lock_guard lock(impl_->mutex_);
        auto it = impl_->sessions_.find(id);
        if (it == impl_->sessions_.end())
        {
            return std::nullopt;
        }

        auto now = session::clock::now();
        auto& stored = it->second;
        if (impl_->is_expired(stored, now))
        {
            impl_->sessions_.erase(it);
            return std::nullopt;
        }
        stored.last_access = now;

        // 返回**值**：别名会让两个携带同一 session_id 的并发请求拿到同一份可变数据，
        // 而 handler 里的 set/remove 不持有 store 的锁，于是同一张 unordered_map
        // 的插入与遍历并发执行。快照的待提交写入为空，它代表已提交的基线。
        return session(stored.id, stored.created, stored.last_access, stored.data);
    }

    void
    memory_session_store::save(std::string_view id, session_writes const& writes)
    {
        std::lock_guard lock(impl_->mutex_);
        auto now = session::clock::now();
        impl_->sweep(now);

        auto it = impl_->sessions_.find(id);
        if (it == impl_->sessions_.end())
        {
            if (impl_->sessions_.size() >= impl_->max_sessions_)
            {
                // 已达上限：先确保没有过期条目可回收，再淘汰最久未访问的一条。
                impl_->erase_expired(now);
                if (impl_->sessions_.size() >= impl_->max_sessions_)
                {
                    impl_->evict_oldest();
                }
            }
            // 先构造条目再插入：两处都用到 id 时，直接写 emplace(key, entry{std::move(key), ...})
            // 会因实参求值顺序未指定而把键 move 成空串。
            auto key = std::string(id);
            impl::entry fresh { key, now, now, {} };
            it = impl_->sessions_.emplace(std::move(key), std::move(fresh)).first;
        }

        // 增量应用而非整体替换：本次请求基于 load() 的基线快照，期间可能已有另一个
        // 请求保存了同一 id，整体替换会把对方的写入整块丢掉（last-writer-wins）。
        impl_->apply(it->second, writes);
        it->second.last_access = now;
    }

    void
    memory_session_store::destroy(std::string_view id)
    {
        std::lock_guard lock(impl_->mutex_);
        impl_->sessions_.erase(std::string(id));
    }

    void
    memory_session_store::set_max_sessions(std::size_t max_sessions)
    {
        std::lock_guard lock(impl_->mutex_);
        impl_->max_sessions_ = max_sessions;
    }

    std::size_t
    memory_session_store::size() const
    {
        std::lock_guard lock(impl_->mutex_);
        return impl_->sessions_.size();
    }

    void
    memory_session_store::cleanup()
    {
        std::lock_guard lock(impl_->mutex_);
        impl_->last_sweep_ = session::clock::now();
        impl_->erase_expired(impl_->last_sweep_);
    }

    // ---- session_middleware ----

    namespace
    {
        constexpr std::string_view session_new_tag = "httplib.session_middleware.new";
    }

    class session_middleware::impl
    {
      public:
        session_config config_;
        std::shared_ptr<session_store> store_;

        static std::string
        generate_id()
        {
            static std::atomic<uint64_t> counter { 0 };
            static thread_local std::mt19937_64 rng(std::random_device {}());
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            auto rand = rng();
            return std::format("{:x}-{:x}-{:x}", now, rand, counter.fetch_add(1));
        }

        static void
        set_cookie(session_config const& cfg, session const& sess, response& resp)
        {
            html::cookie ck;
            ck.name = cfg.cookie_name;
            ck.value = sess.id();
            ck.path = cfg.cookie_path;
            ck.max_age = cfg.max_age;
            ck.http_only = cfg.http_only;
            ck.secure = cfg.secure;
            ck.same_site = cfg.same_site;
            resp.base().insert(field::set_cookie, ck.to_set_cookie_string());
        }
    };

    session_middleware::session_middleware() : impl_(std::make_unique<impl>())
    {
        impl_->store_ = std::make_shared<memory_session_store>(impl_->config_.store_ttl);
    }

    session_middleware::session_middleware(std::shared_ptr<session_store> store) : impl_(std::make_unique<impl>())
    {
        impl_->store_ = std::move(store);
    }

    session_middleware::~session_middleware() = default;

    session_middleware&
    session_middleware::cookie_name(std::string name)
    {
        impl_->config_.cookie_name = std::move(name);
        return *this;
    }

    session_middleware&
    session_middleware::cookie_path(std::string path)
    {
        impl_->config_.cookie_path = std::move(path);
        return *this;
    }

    session_middleware&
    session_middleware::max_age(std::chrono::seconds age)
    {
        impl_->config_.max_age = age;
        return *this;
    }

    session_middleware&
    session_middleware::http_only(bool v)
    {
        impl_->config_.http_only = v;
        return *this;
    }

    session_middleware&
    session_middleware::secure(bool v)
    {
        impl_->config_.secure = v;
        return *this;
    }

    session_middleware&
    session_middleware::same_site_lax()
    {
        impl_->config_.same_site = httplib::html::cookie::same_site_t::lax;
        return *this;
    }

    session_middleware&
    session_middleware::same_site_strict()
    {
        impl_->config_.same_site = httplib::html::cookie::same_site_t::strict;
        return *this;
    }

    session_middleware&
    session_middleware::same_site_none()
    {
        impl_->config_.same_site = httplib::html::cookie::same_site_t::none;
        return *this;
    }

    session_middleware&
    session_middleware::store_ttl(std::chrono::seconds ttl)
    {
        impl_->config_.store_ttl = ttl;
        return *this;
    }

    session_middleware&
    session_middleware::max_sessions(std::size_t n)
    {
        impl_->store_->set_max_sessions(n);
        return *this;
    }

    bool
    session_middleware::before(request& req, response&)
    {
        auto jar = html::cookie_jar::parse(req[field::cookie]);
        auto sid = jar.get(impl_->config_.cookie_name);

        std::shared_ptr<session> sess;
        if (sid)
        {
            if (auto snapshot = impl_->store_->load(*sid))
            {
                sess = std::make_shared<session>(std::move(*snapshot));
            }
        }

        bool is_new = false;
        if (!sess)
        {
            sess = std::make_shared<session>(impl::generate_id());
            is_new = true;
        }

        req.data().store<value_type>(std::move(sess));
        req.data().store<bool>(session_new_tag, std::move(is_new));
        return true;
    }

    bool
    session_middleware::after(request& req, response& resp)
    {
        // 前面的 before 可能被短路（例如同一路由上的限流中间件返回 429），
        // 此时本中间件的 before 从未执行、也没存过 session。fetch 单次加锁完成
        // 判断与取值，has() 后再另行取值之间若有并发 erase() 会漏判。
        auto sess_result = req.data().fetch<value_type>();
        if (!sess_result)
        {
            return true;
        }
        auto sess = sess_result.value();
        if (!sess)
        {
            return true;
        }
        bool is_new = req.data().fetch<bool>(session_new_tag).value_or(false);

        // 只提交本请求真正碰过的键（见 session_store::save 契约）：把整张基线快照
        // 交出去会让并发请求对同一 id 的写入互相覆盖。
        impl_->store_->save(sess->id(), sess->take_pending_writes());

        if (is_new || sess->last_access() - sess->created() < std::chrono::seconds(1))
        {
            impl::set_cookie(impl_->config_, *sess, resp);
        }

        return true;
    }

    std::shared_ptr<session_store>
    session_middleware::store()
    {
        return impl_->store_;
    }

} // namespace httplib::server::middleware
