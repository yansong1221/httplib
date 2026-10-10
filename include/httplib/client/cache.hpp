#pragma once
#include "httplib/client/client_fwd.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

        /// One stored entry: an opaque metadata blob, an optional expiry and
        /// read access to the body. Callers only ever see this interface; how
        /// the bytes are produced (a file, memory, a remote store, ...) and
        /// where the metadata comes from is the backend's business.
        ///
        /// An entry is a live handle: the backend keeps the body readable for
        /// as long as the entry is alive (the disk cache pins its body file by
        /// holding it open), so eviction of the underlying storage does not
        /// invalidate an entry already handed out. Every read still reports
        /// failure instead of returning wrong data.
        class HTTPLIB_API entry
        {
          public:
            virtual ~entry() = default;

            entry(entry const&) = delete;
            entry& operator=(entry const&) = delete;

            /// Opaque, caller-defined bytes. The cache stores and returns them
            /// verbatim; it never parses them.
            virtual std::string_view metadata() const = 0;

            /// Absolute wall-clock deadline after which the entry may be
            /// evicted. `nullopt` means "no per-entry TTL"; the cache then
            /// falls back to its own default max_age.
            virtual std::optional<time_point> expires_at() const = 0;

            /// Byte count observed when the entry was fetched. Never touches
            /// the body.
            virtual std::uint64_t size() const = 0;

            /// Returns every byte of the body, or nullopt when it is
            /// unreadable. An empty body yields "".
            virtual std::optional<std::string> read_all() const = 0;

            /// Writes every byte to `dst` (created or truncated), without
            /// buffering the whole body in memory. Non-atomic: callers that
            /// need atomic publication write to a temporary and rename it
            /// themselves. Returns an error when the body is unreadable.
            virtual std::error_code copy_to_file(fs::path const& dst) const = 0;

          protected:
            entry() = default;
        };

        virtual ~cache() = default;

        cache(cache const&) = delete;
        cache& operator=(cache const&) = delete;
        cache(cache&&) = default;
        cache& operator=(cache&&) = default;

        /// Returns the entry stored for `key`, or nullptr if absent/expired.
        virtual std::unique_ptr<entry> get(std::string_view key) = 0;

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
