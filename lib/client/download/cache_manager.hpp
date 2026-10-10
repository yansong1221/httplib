#pragma once
#include "httplib/client/cache.hpp"
#include "httplib/headers.hpp"
#include "httplib/url/url.hpp"
#include <atomic>
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
            std::string etag;
            std::string last_modified;
            std::string content_type;
            std::string content_disposition;
            /// Time until which the response is fresh (Cache-Control max-age).
            std::optional<std::chrono::system_clock::time_point> fresh_until;
            /// Cache-Control: no-cache -> must revalidate even if fresh.
            bool must_revalidate = false;
        };

        /// A cache hit resolved into the stored entry plus its parsed HTTP
        /// metadata.
        struct cached_entry
        {
            cache::entry entry;
            http_meta meta;
        };

        cache_manager() = default;

        /// Enables caching with `c`; a null `c` disables it.
        void set_cache(std::shared_ptr<cache> c);

        bool enabled() const;
        std::shared_ptr<cache> raw_cache() const;
        /// Fetches the cached entry for `ui`, scoped by `request_headers`
        /// credentials, with its metadata already parsed.
        std::optional<cached_entry> get(url::url_info const& ui, httplib::headers const& request_headers) const;
        /// Stores `body` under the key derived from `ui` + `request_headers`,
        /// with the cacheable metadata extracted from `response_headers`.
        void put(url::url_info const& ui,
                 httplib::headers const& request_headers,
                 httplib::headers const& response_headers,
                 fs::path const& body) const;

        /// Evicts the entry for `ui` under `request_headers`'s credential scope.
        /// Used when a cached body turns out to be unusable (e.g. evicted).
        void remove(url::url_info const& ui, httplib::headers const& request_headers) const;

      private:
        /// Atomic so set_cache() (public API, any thread) can race safely with
        /// the read paths (get/put/enabled/raw_cache) running on the executor.
        std::atomic<std::shared_ptr<cache>> cache_ { nullptr };
    };

} // namespace httplib::client
