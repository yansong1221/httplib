#pragma once
#include "httplib/client/disk_cache.hpp"
#include <boost/beast/http/fields.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace httplib::client
{

    class disk_cache::impl
    {
      public:
        explicit impl(fs::path cache_dir);
        ~impl();

        std::optional<disk_cache::entry> get(std::string_view url);
        void put(std::string_view url, http::fields const& headers, fs::path const& src_body);
        void remove(std::string_view url);
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
        void cleanup_locked();

        fs::path cache_dir_;
        std::uint64_t max_size_ = 512ULL * 1024 * 1024;
        std::chrono::seconds max_age_ = std::chrono::hours(24 * 7);

        mutable std::mutex mutex_;
    };

} // namespace httplib::client
