#include "html/cookie.hpp"
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/util/string_hash.hpp"
#include "memory_store.hpp"
#include <atomic>
#include <mutex>
#include <random>
#include "beast_alias.hpp"

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

    // ---- session ----

    struct session::impl
    {
        std::string id;
        session::time_point created;
        session::time_point last_access;
        util::string_map<std::string> data;
    };

    session::session(std::string id, time_point created)
        : impl_(std::make_unique<impl>(std::move(id), created, created))
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
        impl_->data[std::move(key)] = std::move(value);
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

    // ---- memory_session_store ----

    class memory_session_store::impl
    {
      public:
        using clock = session::clock;
        using time_point = session::time_point;

        std::chrono::seconds ttl_;
        std::mutex mutex_;
        util::string_map<std::shared_ptr<session>> sessions_;

        std::size_t max_sessions_ = 8192;
        time_point last_sweep_ {};

        bool
        is_expired(session const& s) const
        {
            return (clock::now() - s.last_access()) > ttl_;
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
                if ((now - it->second->last_access()) > ttl_)
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
                if (victim == sessions_.end() || it->second->last_access() < victim->second->last_access())
                {
                    victim = it;
                }
            }
            if (victim != sessions_.end())
            {
                sessions_.erase(victim);
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

    std::shared_ptr<session>
    memory_session_store::load(std::string_view id)
    {
        std::lock_guard lock(impl_->mutex_);
        auto it = impl_->sessions_.find(id);
        if (it != impl_->sessions_.end())
        {
            auto s = it->second;
            if (impl_->is_expired(*s))
            {
                impl_->sessions_.erase(it);
                return nullptr;
            }
            s->touch();
            return s;
        }
        return nullptr;
    }

    void
    memory_session_store::save(session const& s)
    {
        std::lock_guard lock(impl_->mutex_);
        auto now = session::clock::now();
        impl_->sweep(now);

        auto it = impl_->sessions_.find(s.id());
        if (it == impl_->sessions_.end() && impl_->sessions_.size() >= impl_->max_sessions_)
        {
            // 已达上限：先确保没有过期条目可回收，再淘汰最久未访问的一条。
            impl_->erase_expired(now);
            if (impl_->sessions_.size() >= impl_->max_sessions_)
            {
                impl_->evict_oldest();
            }
        }
        impl_->sessions_[s.id()] = std::make_shared<session>(s);
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
        auto sess = sid ? impl_->store_->load(*sid) : nullptr;

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
        auto sess = req.data().fetch<value_type>();
        bool is_new = req.data().fetch<bool>(session_new_tag);

        impl_->store_->save(*sess);

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
