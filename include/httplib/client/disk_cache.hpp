#pragma once
#include "httplib/client/cache.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace httplib::client
{

    class HTTPLIB_API disk_cache : public cache
    {
      public:
        explicit disk_cache(fs::path cache_dir);
        ~disk_cache() override;

        disk_cache(disk_cache const&) = delete;
        disk_cache& operator=(disk_cache const&) = delete;
        disk_cache(disk_cache&&) noexcept;
        disk_cache& operator=(disk_cache&&) noexcept;

        std::optional<entry> get(std::string_view url) override;
        void put(std::string_view url, http::fields const& headers, fs::path const& src_body) override;
        void remove(std::string_view url) override;

        void clear();
        void cleanup();

        void set_max_size(std::uint64_t max_bytes);
        void set_max_age(std::chrono::seconds max_age);

        std::uint64_t total_size() const;
        std::size_t entry_count() const;

        fs::path const& directory() const;

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::client
