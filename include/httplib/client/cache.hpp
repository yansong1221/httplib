#pragma once
#include "httplib/client/client_fwd.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::client
{

    /// Generic keyed blob store. The cache knows nothing about HTTP: it maps an
    /// opaque, caller-computed key to a body plus an opaque caller-defined
    /// metadata blob, and it evicts entries based on their expiry and the cache
    /// size policy.
    ///
    /// The key is an arbitrary string chosen by the caller (for a downloader it
    /// is derived from the URL and credentials, but the cache never interprets
    /// it). `metadata` is never parsed by the cache either; the caller is free
    /// to serialize whatever it needs (e.g. HTTP validators) into it.
    class HTTPLIB_API cache
    {
      public:
        using clock = std::chrono::system_clock;
        using time_point = clock::time_point;

        struct entry
        {
            fs::path body_path;
            std::uint64_t body_size = 0;
            /// Opaque, caller-defined bytes. The cache stores and returns them
            /// verbatim.
            std::string metadata;
            /// Absolute wall-clock deadline after which the entry may be
            /// evicted. `nullopt` means "no per-entry TTL"; the cache then falls
            /// back to its own default max_age.
            std::optional<time_point> expires_at;
        };

        virtual ~cache() = default;

        cache(cache const&) = delete;
        cache& operator=(cache const&) = delete;
        cache(cache&&) = default;
        cache& operator=(cache&&) = default;

        /// Returns the entry stored for `key`, or nullopt if absent/expired.
        virtual std::optional<entry> get(std::string_view key) = 0;

        /// Stores `src_body` and `metadata` under `key` (replacing any previous
        /// entry). A missing/invalid `src_body` must leave an existing entry
        /// untouched.
        virtual void put(std::string_view key,
                         fs::path const& src_body,
                         std::string_view metadata = {},
                         std::optional<time_point> expires_at = std::nullopt)
            = 0;

        /// Updates only the metadata/expiry of an existing entry, leaving the
        /// body in place (e.g. after a 304 response). Returns false when there
        /// is no live entry for `key`.
        virtual bool update_metadata(std::string_view key,
                                     std::string_view metadata,
                                     std::optional<time_point> expires_at)
            = 0;

        virtual void remove(std::string_view key) = 0;

      protected:
        cache() = default;
    };

} // namespace httplib::client
