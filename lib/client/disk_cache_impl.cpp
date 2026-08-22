#include "disk_cache_impl.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

namespace httplib::client
{

    namespace
    {
        constexpr std::string_view k_body_file = "body";
        constexpr std::string_view k_meta_file = "meta";

        std::string
        fnv1a_64(std::string_view s)
        {
            std::uint64_t h = 14695981039346656037ULL;
            for (auto c : s)
            {
                h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
                h *= 1099511628211ULL;
            }
            std::ostringstream oss;
            oss << std::hex << std::setfill('0') << std::setw(16) << h;
            return oss.str();
        }

        void
        trim(std::string& s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
            {
                s.erase(0, 1);
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            {
                s.pop_back();
            }
        }

        void
        write_meta(fs::path const& meta_path, http::fields const& headers)
        {
            std::ofstream f(meta_path, std::ios::trunc);
            if (!f.is_open())
            {
                return;
            }
            for (auto const& h : headers)
            {
                f << std::string(h.name_string()) << ": " << std::string(h.value()) << '\n';
            }
        }

        http::fields
        read_meta(fs::path const& meta_path)
        {
            http::fields headers;
            std::ifstream f(meta_path);
            if (!f.is_open())
            {
                return headers;
            }
            std::string line;
            while (std::getline(f, line))
            {
                trim(line);
                if (line.empty())
                {
                    continue;
                }
                auto colon = line.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                auto name = line.substr(0, colon);
                auto value = line.substr(colon + 1);
                trim(name);
                trim(value);
                headers.set(name, value);
            }
            return headers;
        }
    } // namespace

    // =========================================================================
    // disk_cache::impl
    // =========================================================================

    disk_cache::impl::impl(fs::path cache_dir) : cache_dir_(std::move(cache_dir)) {}

    disk_cache::impl::~impl() = default;

    std::string
    disk_cache::impl::hash_url(std::string_view url) const
    {
        return fnv1a_64(url);
    }

    fs::path
    disk_cache::impl::entry_dir(std::string_view hash) const
    {
        return cache_dir_ / hash;
    }

    void
    disk_cache::impl::ensure_cache_dir() const
    {
        std::error_code ec;
        fs::create_directories(cache_dir_, ec);
    }

    void
    disk_cache::impl::set_max_size(std::uint64_t max_bytes)
    {
        std::lock_guard lk(mutex_);
        max_size_ = max_bytes;
    }

    void
    disk_cache::impl::set_max_age(std::chrono::seconds max_age)
    {
        std::lock_guard lk(mutex_);
        max_age_ = max_age;
    }

    std::uint64_t
    disk_cache::impl::total_size() const
    {
        std::lock_guard lk(mutex_);
        std::uint64_t total = 0;
        std::error_code ec;
        for (auto const& de : fs::directory_iterator(cache_dir_, ec))
        {
            if (ec)
            {
                break;
            }
            if (!de.is_directory())
            {
                continue;
            }
            auto body_path = de.path() / k_body_file;
            if (fs::exists(body_path, ec) && !ec)
            {
                total += fs::file_size(body_path, ec);
            }
        }
        return total;
    }

    std::size_t
    disk_cache::impl::entry_count() const
    {
        std::lock_guard lk(mutex_);
        std::size_t count = 0;
        std::error_code ec;
        for (auto const& de : fs::directory_iterator(cache_dir_, ec))
        {
            if (ec)
            {
                break;
            }
            if (de.is_directory())
            {
                ++count;
            }
        }
        return count;
    }

    fs::path const&
    disk_cache::impl::directory() const
    {
        return cache_dir_;
    }

    void
    disk_cache::impl::cleanup_locked()
    {
        auto now = std::chrono::system_clock::now();
        std::uint64_t total_sz = 0;

        struct entry_info
        {
            fs::path dir_path;
            std::uint64_t body_size = 0;
            std::chrono::system_clock::time_point last_write;
        };
        std::vector<entry_info> entries;

        std::error_code ec;
        for (auto const& de : fs::directory_iterator(cache_dir_, ec))
        {
            if (ec)
            {
                break;
            }
            if (!de.is_directory())
            {
                continue;
            }

            auto body_path = de.path() / k_body_file;
            if (!fs::exists(body_path, ec) || ec)
            {
                continue;
            }

            auto sz = fs::file_size(body_path, ec);
            if (ec)
            {
                sz = 0;
            }

            auto lwt = fs::last_write_time(body_path, ec);
            auto tp = std::chrono::clock_cast<std::chrono::system_clock>(lwt);

            if (max_age_.count() > 0)
            {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(now - tp);
                if (age > max_age_)
                {
                    fs::remove_all(de.path(), ec);
                    continue;
                }
            }

            total_sz += sz;
            entries.push_back({ de.path(), sz, tp });
        }

        if (max_size_ > 0 && total_sz > max_size_ && !entries.empty())
        {
            std::sort(entries.begin(),
                      entries.end(),
                      [](entry_info const& a, entry_info const& b) { return a.last_write < b.last_write; });

            for (auto& e : entries)
            {
                if (total_sz <= max_size_)
                {
                    break;
                }
                std::error_code rm_ec;
                fs::remove_all(e.dir_path, rm_ec);
                if (!rm_ec && e.body_size <= total_sz)
                {
                    total_sz -= e.body_size;
                }
            }
        }
    }

    std::optional<disk_cache::entry>
    disk_cache::impl::get(std::string_view url)
    {
        std::lock_guard lk(mutex_);
        ensure_cache_dir();

        // throttled cleanup: run if last cleanup was > 60s ago
        {
            static thread_local auto last_cleanup = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - last_cleanup > std::chrono::seconds(60))
            {
                last_cleanup = now;
                cleanup_locked();
            }
        }

        auto hash = hash_url(url);
        auto edir = entry_dir(hash);
        auto body_path = edir / k_body_file;
        auto meta_path = edir / k_meta_file;

        std::error_code ec;
        if (!fs::exists(body_path, ec) || ec || !fs::exists(meta_path, ec) || ec)
        {
            return std::nullopt;
        }

        auto sz = fs::file_size(body_path, ec);
        if (ec)
        {
            return std::nullopt;
        }

        // check age
        if (max_age_.count() > 0)
        {
            auto lwt = fs::last_write_time(body_path, ec);
            if (!ec)
            {
                auto tp = std::chrono::clock_cast<std::chrono::system_clock>(lwt);
                auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - tp);
                if (age > max_age_)
                {
                    remove(url);
                    cleanup_locked();
                    return std::nullopt;
                }
            }
        }

        disk_cache::entry e;
        e.body_path = body_path;
        e.body_size = static_cast<std::uint64_t>(sz);
        e.headers = read_meta(meta_path);

        // touch mtime for LRU ordering
        fs::last_write_time(body_path, std::filesystem::file_time_type::clock::now(), ec);

        return e;
    }

    void
    disk_cache::impl::put(std::string_view url, http::fields const& headers, fs::path const& src_body)
    {
        std::lock_guard lk(mutex_);
        ensure_cache_dir();

        auto hash = hash_url(url);
        auto edir = entry_dir(hash);
        auto body_path = edir / k_body_file;
        auto meta_path = edir / k_meta_file;

        std::error_code ec;
        fs::remove_all(edir, ec);
        fs::create_directories(edir, ec);
        if (ec)
        {
            return;
        }

        fs::copy_file(src_body, body_path, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            fs::remove_all(edir, ec);
            return;
        }

        write_meta(meta_path, headers);

        cleanup_locked();
    }

    void
    disk_cache::impl::remove(std::string_view url)
    {
        std::lock_guard lk(mutex_);
        auto hash = hash_url(url);
        auto edir = entry_dir(hash);
        std::error_code ec;
        fs::remove_all(edir, ec);
    }

    void
    disk_cache::impl::clear()
    {
        std::lock_guard lk(mutex_);
        std::error_code ec;
        for (auto const& de : fs::directory_iterator(cache_dir_, ec))
        {
            if (ec)
            {
                break;
            }
            fs::remove_all(de.path(), ec);
        }
    }

    void
    disk_cache::impl::cleanup()
    {
        std::lock_guard lk(mutex_);
        ensure_cache_dir();
        cleanup_locked();
    }

    // =========================================================================
    // disk_cache public API
    // =========================================================================

    disk_cache::disk_cache(fs::path cache_dir) : impl_(std::make_unique<impl>(std::move(cache_dir))) {}

    disk_cache::disk_cache(disk_cache&&) noexcept = default;
    disk_cache& disk_cache::operator=(disk_cache&&) noexcept = default;

    disk_cache::~disk_cache() {}

    std::optional<disk_cache::entry>
    disk_cache::get(std::string_view url)
    {
        return impl_->get(url);
    }

    void
    disk_cache::put(std::string_view url, http::fields const& headers, fs::path const& src_body)
    {
        impl_->put(url, headers, src_body);
    }

    void
    disk_cache::remove(std::string_view url)
    {
        impl_->remove(url);
    }

    void
    disk_cache::clear()
    {
        impl_->clear();
    }

    void
    disk_cache::cleanup()
    {
        impl_->cleanup();
    }

    void
    disk_cache::set_max_size(std::uint64_t max_bytes)
    {
        impl_->set_max_size(max_bytes);
    }

    void
    disk_cache::set_max_age(std::chrono::seconds max_age)
    {
        impl_->set_max_age(max_age);
    }

    std::uint64_t
    disk_cache::total_size() const
    {
        return impl_->total_size();
    }

    std::size_t
    disk_cache::entry_count() const
    {
        return impl_->entry_count();
    }

    fs::path const&
    disk_cache::directory() const
    {
        return impl_->directory();
    }

} // namespace httplib::client
