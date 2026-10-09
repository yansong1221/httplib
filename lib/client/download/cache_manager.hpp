#pragma once
#include "httplib/client/cache.hpp"
#include "httplib/headers.hpp"
#include "httplib/url/url.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::client
{

    /// Wraps a generic `cache` with the downloader's HTTP bookkeeping: key
    /// derivation (URL + credential scope), metadata extraction/serialization,
    /// and cacheability policy. The cache itself never interprets any of this.
    class cache_manager
    {
      public:
        /// HTTP-specific cache bookkeeping, serialized into the cache's opaque
        /// metadata blob.
        struct http_meta
        {
            std::string final_url;
            std::string etag;
            std::string last_modified;
            std::string content_type;
            std::string content_disposition;
            /// Time until which the response is fresh (Cache-Control max-age).
            std::optional<std::chrono::system_clock::time_point> fresh_until;
            /// Cache-Control: no-cache -> must revalidate even if fresh.
            bool must_revalidate = false;
        };

        explicit cache_manager(std::shared_ptr<cache> c);

        bool enabled() const;
        std::shared_ptr<cache> raw_cache() const;
        std::optional<cache::entry> get(std::string_view key) const;
        void put(std::string_view key, fs::path const& body, http_meta const& meta) const;

        static std::string make_key(url::url_info const& ui, std::string const& auth_scope);
        static std::string auth_scope(httplib::headers const& headers);
        static http_meta make_meta(httplib::headers const& response,
                                   httplib::headers const& probe,
                                   url::url_info const& final_ui,
                                   bool has_final_ui);
        static std::string serialize_meta(http_meta const& meta);
        static std::optional<http_meta> parse_meta(std::string_view blob);

      private:
        std::shared_ptr<cache> cache_;
    };

} // namespace httplib::client
