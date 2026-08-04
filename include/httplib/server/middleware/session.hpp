#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httplib::server::middleware
{

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
        std::unordered_map<std::string, std::string> const& data() const;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    class HTTPLIB_API session_store
    {
      public:
        virtual ~session_store() = default;
        virtual std::shared_ptr<session> load(std::string_view id) = 0;
        virtual void save(session const& s) = 0;
        virtual void destroy(std::string_view id) = 0;
    };

    class HTTPLIB_API session_middleware
    {
      public:
        static constexpr auto key = "session";
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

        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

        std::shared_ptr<session_store> store();

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
