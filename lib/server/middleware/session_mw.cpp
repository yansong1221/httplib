#include "html/cookie.hpp"
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "memory_store.hpp"
#include <atomic>
#include <mutex>
#include <random>

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

    session::session(std::string id, time_point created) : id_(std::move(id)), created_(created), last_access_(created)
    {
    }

    std::optional<std::string>
    session::get(std::string_view key) const
    {
        auto it = data_.find(std::string(key));
        if (it != data_.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void
    session::set(std::string key, std::string value)
    {
        data_[std::move(key)] = std::move(value);
    }

    bool
    session::has(std::string_view key) const
    {
        return data_.count(std::string(key)) > 0;
    }

    void
    session::remove(std::string_view key)
    {
        data_.erase(std::string(key));
    }

    bool
    session::empty() const
    {
        return data_.empty();
    }

    std::unordered_map<std::string, std::string> const&
    session::data() const
    {
        return data_;
    }

    // ---- memory_session_store ----

    class memory_session_store::impl
    {
      public:
        using clock = session::clock;
        using time_point = session::time_point;

        std::chrono::seconds ttl_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<session>> sessions_;

        bool
        is_expired(session const& s) const
        {
            return (clock::now() - s.last_access()) > ttl_;
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
        auto it = impl_->sessions_.find(std::string(id));
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
        impl_->sessions_[s.id()] = std::make_shared<session>(s);
    }

    void
    memory_session_store::destroy(std::string_view id)
    {
        std::lock_guard lock(impl_->mutex_);
        impl_->sessions_.erase(std::string(id));
    }

    void
    memory_session_store::cleanup()
    {
        std::lock_guard lock(impl_->mutex_);
        for (auto it = impl_->sessions_.begin(); it != impl_->sessions_.end();)
        {
            if (impl_->is_expired(*it->second))
            {
                it = impl_->sessions_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // ---- session_middleware ----

    class session_middleware::impl
    {
      public:
        session_config config_;
        std::shared_ptr<session_store> store_;
        bool is_new_ = true;

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
            resp.base().insert(http::field::set_cookie, ck.to_set_cookie_string());
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

    bool
    session_middleware::before(request& req, response&)
    {
        auto jar = html::cookie_jar::parse(req[http::field::cookie]);
        auto sid = jar.get(impl_->config_.cookie_name);
        auto sess = sid ? impl_->store_->load(*sid) : nullptr;

        if (!sess)
        {
            sess = std::make_shared<session>(impl::generate_id());
            impl_->is_new_ = true;
        }
        else
        {
            impl_->is_new_ = false;
        }

        req.set_session(std::move(sess));
        return true;
    }

    bool
    session_middleware::after(request& req, response& resp)
    {
        auto sess = req.session();

        impl_->store_->save(*sess);

        if (impl_->is_new_ || sess->last_access() - sess->created() < std::chrono::seconds(1))
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
