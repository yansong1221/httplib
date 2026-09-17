#pragma once
#include "httplib/client/disk_cache.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::client
{

    /// Best-effort advisory lock guarding a cache directory against concurrent
    /// writers (one cache instance per directory). A failed acquisition is not
    /// fatal; the owning disk_cache simply proceeds without cross-process
    /// exclusion. The lock file is never removed by the cache itself.
    struct cache_lock
    {
        explicit cache_lock(fs::path const& lock_path);
        ~cache_lock();
        cache_lock(cache_lock const&) = delete;
        cache_lock& operator=(cache_lock const&) = delete;

        bool
        acquired() const noexcept
        {
            return acquired_;
        }

      private:
        void* handle_ = nullptr;
        bool acquired_ = false;
    };

    class disk_cache::impl
    {
      public:
        explicit impl(fs::path cache_dir);
        ~impl();

        std::optional<disk_cache::entry> get(std::string_view key);
        void put(std::string_view key,
                 fs::path const& src_body,
                 std::string_view metadata,
                 std::optional<disk_cache::time_point> expires_at);
        bool update_metadata(std::string_view key,
                             std::string_view metadata,
                             std::optional<disk_cache::time_point> expires_at);
        void remove(std::string_view key);
        void clear();
        void cleanup();

        void set_max_size(std::uint64_t max_bytes);
        void set_max_age(std::chrono::seconds max_age);

        std::uint64_t total_size() const;
        std::size_t entry_count() const;
        fs::path const& directory() const;

      private:
        std::string hash_url(std::string_view url) const;
        fs::path entry_dir(std::string_view hash) const;
        void ensure_cache_dir() const;
        std::optional<disk_cache::time_point> read_expires(fs::path const& edir) const;
        int read_version(fs::path const& edir) const;
        void cleanup_locked();

        fs::path cache_dir_;
        std::uint64_t max_size_ = 512ULL * 1024 * 1024;
        std::chrono::seconds max_age_ = std::chrono::hours(24 * 7);
        /// Timestamp of the last throttled cleanup. Shared across threads (unlike
        /// a thread_local), so concurrent users of one cache clean up coherently.
        std::chrono::steady_clock::time_point last_cleanup_ = std::chrono::steady_clock::now();

        std::unique_ptr<cache_lock> lock_;

        mutable std::mutex mutex_;
    };

} // namespace httplib::client
