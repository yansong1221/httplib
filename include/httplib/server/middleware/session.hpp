#pragma once
#include "httplib/config.hpp"
#include "httplib/html/cookie.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

namespace httplib::server::middleware {

class session
{
public:
    using clock      = std::chrono::system_clock;
    using time_point = clock::time_point;

    explicit session(std::string id, time_point created = clock::now())
        : id_(std::move(id))
        , created_(created)
        , last_access_(created)
    {
    }

    const std::string& id() const { return id_; }
    time_point created() const { return created_; }
    time_point last_access() const { return last_access_; }

    void touch() { last_access_ = clock::now(); }

    std::optional<std::string> get(std::string_view key) const
    {
        auto it = data_.find(std::string(key));
        if (it != data_.end())
            return it->second;
        return std::nullopt;
    }

    void set(std::string key, std::string value) { data_[std::move(key)] = std::move(value); }

    bool has(std::string_view key) const { return data_.count(std::string(key)) > 0; }

    void remove(std::string_view key) { data_.erase(std::string(key)); }

    bool empty() const { return data_.empty(); }

    const std::unordered_map<std::string, std::string>& data() const { return data_; }

private:
    std::string id_;
    time_point created_;
    time_point last_access_;
    std::unordered_map<std::string, std::string> data_;
};

class session_store
{
public:
    virtual ~session_store() = default;

    virtual std::shared_ptr<session> load(std::string_view id) = 0;
    virtual void save(const session& s)                        = 0;
    virtual void destroy(std::string_view id)                  = 0;
};

class memory_session_store : public session_store
{
    using clock      = session::clock;
    using time_point = session::time_point;

public:
    explicit memory_session_store(std::chrono::seconds ttl = std::chrono::hours(24))
        : ttl_(ttl)
    {
    }

    std::shared_ptr<session> load(std::string_view id) override
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(std::string(id));
        if (it != sessions_.end()) {
            auto s = it->second;
            if (is_expired(*s)) {
                sessions_.erase(it);
                return nullptr;
            }
            s->touch();
            return s;
        }
        return nullptr;
    }

    void save(const session& s) override
    {
        std::lock_guard lock(mutex_);
        sessions_[s.id()] = std::make_shared<session>(s);
    }

    void destroy(std::string_view id) override
    {
        std::lock_guard lock(mutex_);
        sessions_.erase(std::string(id));
    }

    void cleanup()
    {
        std::lock_guard lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (is_expired(*it->second))
                it = sessions_.erase(it);
            else
                ++it;
        }
    }

private:
    bool is_expired(const session& s) const
    {
        return (clock::now() - s.last_access()) > ttl_;
    }

    std::chrono::seconds ttl_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<session>> sessions_;
};

struct session_config {
    std::string cookie_name = "session_id";
    std::string cookie_path = "/";
    std::chrono::seconds max_age{0};
    bool http_only = true;
    bool secure    = false;
    html::cookie::same_site_t same_site = html::cookie::same_site_t::lax;
    std::chrono::seconds store_ttl      = std::chrono::hours(24);
};

class session_middleware
{
public:
    explicit session_middleware(session_config config = {})
        : config_(std::move(config))
        , store_(std::make_shared<memory_session_store>(config_.store_ttl))
    {
    }

    explicit session_middleware(std::shared_ptr<session_store> store,
                               session_config config = {})
        : config_(std::move(config))
        , store_(std::move(store))
    {
    }

    bool before(request& req, response&)
    {
        auto jar    = html::cookie_jar::parse(req.base()[http::field::cookie]);
        auto sid    = jar.get(config_.cookie_name);
        auto sess   = sid ? store_->load(*sid) : nullptr;

        if (!sess) {
            sess    = std::make_shared<session>(generate_id());
            is_new_ = true;
        }
        else {
            is_new_ = false;
        }

        req.set_session(std::move(sess));
        return true;
    }

    bool after(request& req, response& resp)
    {
        auto sess = req.session();

        store_->save(*sess);

        if (is_new_ || sess->last_access() - sess->created() < std::chrono::seconds(1)) {
            html::cookie ck;
            ck.name      = config_.cookie_name;
            ck.value     = sess->id();
            ck.path      = config_.cookie_path;
            ck.max_age   = config_.max_age;
            ck.http_only = config_.http_only;
            ck.secure    = config_.secure;
            ck.same_site = config_.same_site;
            resp.base().insert(http::field::set_cookie, ck.to_set_cookie_string());
        }

        return true;
    }

    std::shared_ptr<session_store> store() { return store_; }

private:
    static std::string generate_id()
    {
        static std::atomic<uint64_t> counter{0};
        static thread_local std::mt19937_64 rng(std::random_device{}());
        auto now  = std::chrono::system_clock::now().time_since_epoch().count();
        auto rand = rng();
        return std::format("{:x}-{:x}-{:x}", now, rand, counter.fetch_add(1));
    }

    session_config config_;
    std::shared_ptr<session_store> store_;
    bool is_new_ = true;
};

} // namespace httplib::server::middleware
