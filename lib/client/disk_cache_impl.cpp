#include "disk_cache_impl.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace httplib::client
{

    namespace
    {
        constexpr std::string_view k_body_file = "body";
        constexpr std::string_view k_meta_file = "meta";
        constexpr std::string_view k_expires_file = "expires";
        constexpr std::string_view k_version_file = "version";
        std::string const k_lock_file = ".lock";

        /// On-disk entry layout version. Bump this whenever the meaning of the
        /// files inside an entry directory changes. Entries written by another
        /// version (or without a version file) are treated as cache misses and
        /// removed lazily, so a layout change never serves stale/corrupt data.
        constexpr int k_format_version = 1;

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

        bool
        is_hex_name(std::string_view name, std::size_t len)
        {
            if (name.size() != len)
            {
                return false;
            }
            for (auto c : name)
            {
                if (!std::isxdigit(static_cast<unsigned char>(c)))
                {
                    return false;
                }
            }
            return true;
        }

        /// Enumerate every entry directory. Supports both the sharded layout
        /// (<cache>/<xx>/<hash>/) and any legacy flat layout (<cache>/<hash>/).
        /// The reserved lock file and temp directories are ignored by name shape.
        std::vector<fs::path>
        collect_entry_dirs(fs::path const& cache_dir)
        {
            std::vector<fs::path> out;
            std::error_code ec;
            for (auto const& top : fs::directory_iterator(cache_dir, ec))
            {
                if (ec)
                {
                    break;
                }
                std::error_code tec;
                if (!top.is_directory(tec) || tec)
                {
                    continue;
                }
                auto name = top.path().filename().string();
                if (is_hex_name(name, 16))
                {
                    out.push_back(top.path());
                    continue;
                }
                if (is_hex_name(name, 2))
                {
                    std::error_code lec;
                    for (auto const& leaf : fs::directory_iterator(top.path(), lec))
                    {
                        if (lec)
                        {
                            break;
                        }
                        std::error_code d_ec;
                        if (!leaf.is_directory(d_ec) || d_ec)
                        {
                            continue;
                        }
                        auto lname = leaf.path().filename().string();
                        if (is_hex_name(lname, 16))
                        {
                            out.push_back(leaf.path());
                        }
                    }
                }
            }
            return out;
        }

        std::string
        read_file_string(fs::path const& p)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open())
            {
                return {};
            }
            return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
        }

        bool
        write_file_string(fs::path const& p, std::string_view data)
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            if (!f.is_open())
            {
                return false;
            }
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
            return static_cast<bool>(f);
        }

        std::int64_t
        to_unix_seconds(cache::time_point tp)
        {
            return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
        }

        cache::time_point
        from_unix_seconds(std::int64_t s)
        {
            return cache::time_point(std::chrono::seconds(s));
        }

        bool
        expires_equal(std::optional<cache::time_point> const& a, std::optional<cache::time_point> const& b)
        {
            if (a.has_value() != b.has_value())
            {
                return false;
            }
            if (!a.has_value())
            {
                return true;
            }
            return to_unix_seconds(*a) == to_unix_seconds(*b);
        }
    } // namespace

    // =========================================================================
    // cache_lock
    // =========================================================================

    cache_lock::cache_lock(fs::path const& lock_path)
    {
#if defined(_WIN32)
        HANDLE h = CreateFileW(lock_path.wstring().c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        OVERLAPPED ov {};
        if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
        {
            handle_ = h;
            acquired_ = true;
        }
        else
        {
            CloseHandle(h);
        }
#else
        int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0)
        {
            return;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
            acquired_ = true;
        }
        else
        {
            ::close(fd);
        }
#endif
    }

    cache_lock::~cache_lock()
    {
#if defined(_WIN32)
        if (handle_)
        {
            auto h = static_cast<HANDLE>(handle_);
            OVERLAPPED ov {};
            UnlockFileEx(h, 0, 1, 0, &ov);
            CloseHandle(h);
        }
#else
        if (handle_)
        {
            int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
#endif
    }

    // =========================================================================
    // disk_cache::impl
    // =========================================================================

    disk_cache::impl::impl(fs::path cache_dir) : cache_dir_(std::move(cache_dir))
    {
        ensure_cache_dir();
        lock_ = std::make_unique<cache_lock>(cache_dir_ / k_lock_file);
    }

    disk_cache::impl::~impl() = default;

    std::string
    disk_cache::impl::hash_url(std::string_view url) const
    {
        return fnv1a_64(url);
    }

    fs::path
    disk_cache::impl::entry_dir(std::string_view hash) const
    {
        return cache_dir_ / std::string(hash.substr(0, 2)) / std::string(hash);
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

    std::optional<disk_cache::time_point>
    disk_cache::impl::read_expires(fs::path const& edir) const
    {
        auto raw = read_file_string(edir / k_expires_file);
        if (raw.empty())
        {
            return std::nullopt;
        }
        try
        {
            return from_unix_seconds(std::stoll(raw));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    int
    disk_cache::impl::read_version(fs::path const& edir) const
    {
        auto raw = read_file_string(edir / k_version_file);
        if (raw.empty())
        {
            return 0;
        }
        try
        {
            return std::stoi(raw);
        }
        catch (...)
        {
            return 0;
        }
    }

    std::uint64_t
    disk_cache::impl::total_size() const
    {
        std::lock_guard lk(mutex_);
        std::uint64_t total = 0;
        for (auto const& edir : collect_entry_dirs(cache_dir_))
        {
            std::error_code ec;
            auto body = edir / k_body_file;
            if (fs::exists(body, ec) && !ec)
            {
                total += fs::file_size(body, ec);
            }
        }
        return total;
    }

    std::size_t
    disk_cache::impl::entry_count() const
    {
        std::lock_guard lk(mutex_);
        std::size_t count = 0;
        for (auto const& edir : collect_entry_dirs(cache_dir_))
        {
            std::error_code ec;
            if (fs::exists(edir / k_body_file, ec) && !ec)
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
        // Purge staging directories left behind by an interrupted put.
        {
            std::error_code ec;
            for (auto const& top : fs::directory_iterator(cache_dir_, ec))
            {
                if (ec)
                {
                    break;
                }
                std::error_code tec;
                if (!top.is_directory(tec) || tec)
                {
                    continue;
                }
                auto name = top.path().filename().string();
                if (is_hex_name(name, 2))
                {
                    std::error_code lec;
                    for (auto const& leaf : fs::directory_iterator(top.path(), lec))
                    {
                        if (lec)
                        {
                            break;
                        }
                        if (leaf.path().filename().string().ends_with(".tmp"))
                        {
                            std::error_code rm_ec;
                            fs::remove_all(leaf.path(), rm_ec);
                        }
                    }
                }
                else if (name.ends_with(".tmp"))
                {
                    std::error_code rm_ec;
                    fs::remove_all(top.path(), rm_ec);
                }
            }
        }

        auto now = cache::clock::now();

        struct entry_info
        {
            fs::path dir_path;
            std::uint64_t body_size = 0;
            cache::time_point last_write;
        };
        std::vector<entry_info> entries;
        std::uint64_t total_sz = 0;

        for (auto const& edir : collect_entry_dirs(cache_dir_))
        {
            auto body_path = edir / k_body_file;
            std::error_code ec;
            if (!fs::exists(body_path, ec) || ec || read_version(edir) != k_format_version)
            {
                // No body, or an entry from an incompatible format version:
                // drop it so it can never be served.
                std::error_code rm_ec;
                fs::remove_all(edir, rm_ec);
                continue;
            }

            auto sz = fs::file_size(body_path, ec);
            if (ec)
            {
                sz = 0;
            }

            auto exp = read_expires(edir);
            if (exp && now >= *exp)
            {
                std::error_code rm_ec;
                fs::remove_all(edir, rm_ec);
                continue;
            }

            auto lwt = fs::last_write_time(body_path, ec);
            auto tp = ec ? now : std::chrono::clock_cast<cache::clock>(lwt);

            if (!exp && max_age_.count() > 0)
            {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(now - tp);
                if (age > max_age_)
                {
                    std::error_code rm_ec;
                    fs::remove_all(edir, rm_ec);
                    continue;
                }
            }

            total_sz += sz;
            entries.push_back({ edir, sz, tp });
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
    disk_cache::impl::get(std::string_view key)
    {
        std::lock_guard lk(mutex_);
        ensure_cache_dir();

        // throttled cleanup: run if last cleanup was > 60s ago.
        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_cleanup_ > std::chrono::seconds(60))
            {
                last_cleanup_ = now;
                cleanup_locked();
            }
        }

        auto hash = hash_url(key);
        auto edir = entry_dir(hash);
        auto body_path = edir / k_body_file;

        std::error_code ec;
        if (!fs::exists(body_path, ec) || ec)
        {
            return std::nullopt;
        }

        if (read_version(edir) != k_format_version)
        {
            // Entry written by an incompatible layout version: evict and miss.
            std::error_code rm_ec;
            fs::remove_all(edir, rm_ec);
            return std::nullopt;
        }

        auto sz = fs::file_size(body_path, ec);
        if (ec)
        {
            return std::nullopt;
        }

        auto exp = read_expires(edir);
        if (exp && cache::clock::now() >= *exp)
        {
            // Remove directly: mutex_ is already held, so calling the public
            // remove() here would self-deadlock on a non-recursive mutex.
            std::error_code rm_ec;
            fs::remove_all(edir, rm_ec);
            return std::nullopt;
        }

        if (!exp && max_age_.count() > 0)
        {
            auto lwt = fs::last_write_time(body_path, ec);
            if (!ec)
            {
                auto tp = std::chrono::clock_cast<cache::clock>(lwt);
                auto age = std::chrono::duration_cast<std::chrono::seconds>(cache::clock::now() - tp);
                if (age > max_age_)
                {
                    std::error_code rm_ec;
                    fs::remove_all(edir, rm_ec);
                    return std::nullopt;
                }
            }
        }

        disk_cache::entry e;
        e.body_path = body_path;
        e.body_size = static_cast<std::uint64_t>(sz);
        e.metadata = read_file_string(edir / k_meta_file);
        e.expires_at = exp;

        // touch mtime for LRU ordering
        std::error_code touch_ec;
        fs::last_write_time(body_path, fs::file_time_type::clock::now(), touch_ec);

        return e;
    }

    void
    disk_cache::impl::put(std::string_view key,
                          fs::path const& src_body,
                          std::string_view metadata,
                          std::optional<disk_cache::time_point> expires_at)
    {
        std::lock_guard lk(mutex_);
        ensure_cache_dir();

        // Never let a missing/invalid source clobber an existing good entry.
        std::error_code ec;
        if (!fs::exists(src_body, ec) || ec)
        {
            return;
        }
        auto src_size = fs::file_size(src_body, ec);
        if (ec)
        {
            return;
        }

        auto hash = hash_url(key);
        auto edir = entry_dir(hash);
        auto body_path = edir / k_body_file;
        auto meta_path = edir / k_meta_file;

        // Unchanged fast-path: skip the copy when the stored body and metadata
        // already match, only refreshing the LRU timestamp.
        if (fs::exists(body_path, ec) && !ec && read_version(edir) == k_format_version)
        {
            auto existing_size = fs::file_size(body_path, ec);
            if (!ec && existing_size == src_size && read_file_string(meta_path) == metadata
                && expires_equal(read_expires(edir), expires_at))
            {
                std::error_code touch_ec;
                fs::last_write_time(body_path, fs::file_time_type::clock::now(), touch_ec);
                return;
            }
        }

        // Stage the whole entry in a temp directory, then swap it in. This makes
        // the replacement atomic at directory granularity: a reader either sees
        // the old entry or the new one, never a half-written body/meta pair.
        auto tmp_dir = edir;
        tmp_dir += ".tmp";

        fs::create_directories(edir.parent_path(), ec);
        if (ec)
        {
            return;
        }
        {
            std::error_code rm_ec;
            fs::remove_all(tmp_dir, rm_ec);
        }
        fs::create_directories(tmp_dir, ec);
        if (ec)
        {
            return;
        }

        fs::copy_file(src_body, tmp_dir / k_body_file, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            std::error_code rm_ec;
            fs::remove_all(tmp_dir, rm_ec);
            return;
        }
        if (!write_file_string(tmp_dir / k_meta_file, metadata))
        {
            std::error_code rm_ec;
            fs::remove_all(tmp_dir, rm_ec);
            return;
        }
        if (expires_at.has_value())
        {
            if (!write_file_string(tmp_dir / k_expires_file, std::to_string(to_unix_seconds(*expires_at))))
            {
                std::error_code rm_ec;
                fs::remove_all(tmp_dir, rm_ec);
                return;
            }
        }
        if (!write_file_string(tmp_dir / k_version_file, std::to_string(k_format_version)))
        {
            std::error_code rm_ec;
            fs::remove_all(tmp_dir, rm_ec);
            return;
        }

        {
            std::error_code rm_ec;
            fs::remove_all(edir, rm_ec);
        }
        std::error_code rn_ec;
        fs::rename(tmp_dir, edir, rn_ec);
        if (rn_ec)
        {
            std::error_code rm_ec;
            fs::remove_all(tmp_dir, rm_ec);
            return;
        }

        cleanup_locked();
    }

    bool
    disk_cache::impl::update_metadata(std::string_view key,
                                      std::string_view metadata,
                                      std::optional<disk_cache::time_point> expires_at)
    {
        std::lock_guard lk(mutex_);
        ensure_cache_dir();

        auto hash = hash_url(key);
        auto edir = entry_dir(hash);
        auto body_path = edir / k_body_file;

        std::error_code ec;
        if (!fs::exists(body_path, ec) || ec)
        {
            return false;
        }
        if (read_version(edir) != k_format_version)
        {
            std::error_code rm_ec;
            fs::remove_all(edir, rm_ec);
            return false;
        }

        auto exp = read_expires(edir);
        if (exp && cache::clock::now() >= *exp)
        {
            std::error_code rm_ec;
            fs::remove_all(edir, rm_ec);
            return false;
        }

        auto meta_tmp = edir / "meta.tmp";
        if (!write_file_string(meta_tmp, metadata))
        {
            std::error_code rm_ec;
            fs::remove(meta_tmp, rm_ec);
            return false;
        }
        {
            std::error_code rm_ec;
            fs::remove(edir / k_meta_file, rm_ec);
        }
        std::error_code rn_ec;
        fs::rename(meta_tmp, edir / k_meta_file, rn_ec);
        if (rn_ec)
        {
            std::error_code rm_ec;
            fs::remove(meta_tmp, rm_ec);
            return false;
        }

        if (expires_at.has_value())
        {
            auto expires_tmp = edir / "expires.tmp";
            if (write_file_string(expires_tmp, std::to_string(to_unix_seconds(*expires_at))))
            {
                std::error_code rm_ec;
                fs::remove(edir / k_expires_file, rm_ec);
                std::error_code rn_ec2;
                fs::rename(expires_tmp, edir / k_expires_file, rn_ec2);
                if (rn_ec2)
                {
                    std::error_code rm_tmp;
                    fs::remove(expires_tmp, rm_tmp);
                }
            }
        }
        else
        {
            std::error_code rm_ec;
            fs::remove(edir / k_expires_file, rm_ec);
        }

        std::error_code touch_ec;
        fs::last_write_time(body_path, fs::file_time_type::clock::now(), touch_ec);
        return true;
    }

    void
    disk_cache::impl::remove(std::string_view key)
    {
        std::lock_guard lk(mutex_);
        auto hash = hash_url(key);
        std::error_code ec;
        fs::remove_all(entry_dir(hash), ec);
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
            if (de.path().filename().string() == k_lock_file)
            {
                continue;
            }
            std::error_code rm_ec;
            fs::remove_all(de.path(), rm_ec);
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
    disk_cache::get(std::string_view key)
    {
        return impl_->get(key);
    }

    void
    disk_cache::put(std::string_view key,
                    fs::path const& src_body,
                    std::string_view metadata,
                    std::optional<time_point> expires_at)
    {
        impl_->put(key, src_body, metadata, expires_at);
    }

    bool
    disk_cache::update_metadata(std::string_view key, std::string_view metadata, std::optional<time_point> expires_at)
    {
        return impl_->update_metadata(key, metadata, expires_at);
    }

    void
    disk_cache::remove(std::string_view key)
    {
        impl_->remove(key);
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
