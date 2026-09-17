#include "downloader_impl.h"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/when_all.hpp"
#include "redirect_util.hpp"
#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/url.hpp>
#include <format>
#include <fstream>
#include <stdexcept>

namespace httplib::client
{

    namespace
    {
        constexpr std::size_t kReadBufSize = 64 * 1024;

        std::string
        fnv1a_hex(std::string_view s)
        {
            std::uint64_t h = 14695981039346656037ULL;
            for (auto c : s)
            {
                h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
                h *= 1099511628211ULL;
            }
            return std::format("{:016x}", h);
        }

        bool
        ascii_iequals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                auto ca = static_cast<unsigned char>(a[i]);
                auto cb = static_cast<unsigned char>(b[i]);
                if (ca >= 'A' && ca <= 'Z')
                {
                    ca = static_cast<unsigned char>(ca - 'A' + 'a');
                }
                if (cb >= 'A' && cb <= 'Z')
                {
                    cb = static_cast<unsigned char>(cb - 'A' + 'a');
                }
                if (ca != cb)
                {
                    return false;
                }
            }
            return true;
        }

        /// True if the comma-separated `value` contains `token` as a directive
        /// name, case-insensitively (ignoring any `=value` suffix).
        bool
        header_has_token(std::string_view value, std::string_view token)
        {
            std::size_t i = 0;
            while (i <= value.size())
            {
                auto next = value.find(',', i);
                auto end = (next == std::string_view::npos) ? value.size() : next;
                auto part = value.substr(i, end - i);
                while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
                {
                    part.remove_prefix(1);
                }
                while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
                {
                    part.remove_suffix(1);
                }
                if (auto eq = part.find('='); eq != std::string_view::npos)
                {
                    part = part.substr(0, eq);
                    while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
                    {
                        part.remove_suffix(1);
                    }
                }
                if (ascii_iequals(part, token))
                {
                    return true;
                }
                if (next == std::string_view::npos)
                {
                    break;
                }
                i = next + 1;
            }
            return false;
        }

        /// Extract the integer value of a `name=value` directive from a
        /// comma-separated Cache-Control value (e.g. max-age=3600).
        std::optional<std::int64_t>
        header_directive_int(std::string_view value, std::string_view name)
        {
            std::size_t i = 0;
            while (i <= value.size())
            {
                auto next = value.find(',', i);
                auto end = (next == std::string_view::npos) ? value.size() : next;
                auto part = value.substr(i, end - i);
                while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
                {
                    part.remove_prefix(1);
                }
                auto eq = part.find('=');
                if (eq != std::string_view::npos)
                {
                    auto key = part.substr(0, eq);
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                    {
                        key.remove_suffix(1);
                    }
                    if (ascii_iequals(key, name))
                    {
                        auto val = part.substr(eq + 1);
                        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                        {
                            val.remove_prefix(1);
                        }
                        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
                        {
                            val.remove_suffix(1);
                        }
                        try
                        {
                            return std::stoll(std::string(val));
                        }
                        catch (...)
                        {
                            return std::nullopt;
                        }
                    }
                }
                if (next == std::string_view::npos)
                {
                    break;
                }
                i = next + 1;
            }
            return std::nullopt;
        }

        std::uint64_t
        parse_content_length(http::fields const& headers)
        {
            auto cl = headers[http::field::content_length];
            if (!cl.empty())
            {
                try
                {
                    return std::stoull(std::string(cl));
                }
                catch (...)
                {
                }
            }
            return 0;
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
    } // namespace

    downloader::impl::url_info
    downloader::impl::parse_url(std::string_view url)
    {
        url_info ui;
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            throw std::invalid_argument("downloader: invalid url");
        }
        auto const& u = *r;
        ui.host = u.host();
        ui.port = u.port_number() ? u.port_number() : (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        ui.ssl = u.scheme_id() == boost::urls::scheme::https;
        std::string ep(u.encoded_path().data(), u.encoded_path().size());
        ui.path = ep.empty() ? "/" : ep;
        if (u.has_query())
        {
            ui.path += "?";
            ui.path += u.encoded_query();
        }
        return ui;
    }

    std::string
    downloader::impl::make_url_string(url_info const& ui)
    {
        std::string key;
        key.reserve(ui.host.size() + ui.path.size() + 32);
        key.append(ui.ssl ? "https://" : "http://");
        key.append(ui.host);
        if ((ui.ssl && ui.port != 443) || (!ui.ssl && ui.port != 80))
        {
            key.push_back(':');
            key.append(std::to_string(ui.port));
        }
        key.append(ui.path);
        return key;
    }

    std::string
    downloader::impl::cache_auth_scope(http::fields const& headers)
    {
        // Fold the credential-bearing headers into a stable digest. Raw secrets
        // are never stored on disk (the digest is what lands in cache keys and
        // sidecar state files).
        std::string material;
        auto append = [&](http::field f)
        {
            auto v = headers[f];
            if (!v.empty())
            {
                material.append(http::to_string(f).data(), http::to_string(f).size());
                material.push_back(':');
                material.append(v.data(), v.size());
                material.push_back('\n');
            }
        };
        append(http::field::authorization);
        append(http::field::proxy_authorization);
        append(http::field::cookie);
        append(http::field::cookie2);
        if (material.empty())
        {
            return {};
        }
        return fnv1a_hex(material);
    }

    std::string
    downloader::impl::make_cache_key(url_info const& ui) const
    {
        auto key = make_url_string(ui);
        if (!auth_scope_.empty())
        {
            key.append("|auth=");
            key.append(auth_scope_);
        }
        return key;
    }

    bool
    downloader::impl::response_is_cacheable(http::fields const& headers)
    {
        // `no-store` forbids persisting the response; `Vary: *` means the
        // response cannot be selected by request, so caching it is unsafe.
        if (header_has_token(headers[http::field::cache_control], "no-store"))
        {
            return false;
        }
        if (header_has_token(headers[http::field::vary], "*"))
        {
            return false;
        }
        return true;
    }

    downloader::impl::http_meta
    downloader::impl::make_http_meta(http::fields const& response,
                                     http::fields const& probe,
                                     url_info const& final_ui,
                                     bool has_final_ui)
    {
        // Explicit whitelist of HTTP bookkeeping the downloader needs; the rest
        // (hop-by-hop headers, partial-response framing) is discarded.
        http_meta meta;
        auto take = [&](http::field f) -> std::string
        {
            auto v = response[f];
            if (v.empty())
            {
                v = probe[f];
            }
            return std::string(v);
        };
        meta.content_type = take(http::field::content_type);
        meta.content_disposition = take(http::field::content_disposition);
        meta.etag = take(http::field::etag);
        meta.last_modified = take(http::field::last_modified);
        if (has_final_ui)
        {
            meta.final_url = make_url_string(final_ui);
        }

        std::string cache_control = take(http::field::cache_control);
        meta.must_revalidate = header_has_token(cache_control, "no-cache");

        if (auto max_age = header_directive_int(cache_control, "max-age"); max_age && *max_age >= 0)
        {
            std::int64_t age = 0;
            if (auto age_header = response[http::field::age]; !age_header.empty())
            {
                try
                {
                    age = std::stoll(std::string(age_header));
                }
                catch (...)
                {
                    age = 0;
                }
            }
            auto lifetime = std::chrono::seconds(std::max<std::int64_t>(0, *max_age - age));
            meta.fresh_until = std::chrono::system_clock::now() + lifetime;
        }
        return meta;
    }

    std::string
    downloader::impl::serialize_http_meta(http_meta const& meta)
    {
        std::string out;
        auto put = [&](std::string_view key, std::string const& value)
        {
            if (!value.empty())
            {
                out.append(key);
                out.push_back('=');
                out.append(value);
                out.push_back('\n');
            }
        };
        put("final_url", meta.final_url);
        put("etag", meta.etag);
        put("last_modified", meta.last_modified);
        put("content_type", meta.content_type);
        put("content_disposition", meta.content_disposition);
        if (meta.fresh_until.has_value())
        {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(meta.fresh_until->time_since_epoch());
            out.append("fresh_until=");
            out.append(std::to_string(secs.count()));
            out.push_back('\n');
        }
        if (meta.must_revalidate)
        {
            out.append("must_revalidate=1\n");
        }
        return out;
    }

    std::optional<downloader::impl::http_meta>
    downloader::impl::parse_http_meta(std::string_view blob)
    {
        if (blob.empty())
        {
            return std::nullopt;
        }
        http_meta meta;
        std::size_t pos = 0;
        while (pos < blob.size())
        {
            auto nl = blob.find('\n', pos);
            auto end = (nl == std::string_view::npos) ? blob.size() : nl;
            std::string line(blob.substr(pos, end - pos));
            pos = (nl == std::string_view::npos) ? blob.size() : nl + 1;

            trim(line);
            if (line.empty())
            {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            if (key == "final_url")
            {
                meta.final_url = val;
            }
            else if (key == "etag")
            {
                meta.etag = val;
            }
            else if (key == "last_modified")
            {
                meta.last_modified = val;
            }
            else if (key == "content_type")
            {
                meta.content_type = val;
            }
            else if (key == "content_disposition")
            {
                meta.content_disposition = val;
            }
            else if (key == "fresh_until")
            {
                try
                {
                    meta.fresh_until = std::chrono::system_clock::time_point(std::chrono::seconds(std::stoll(val)));
                }
                catch (...)
                {
                }
            }
            else if (key == "must_revalidate")
            {
                meta.must_revalidate = (val == "1");
            }
        }
        return meta;
    }

    std::uint64_t
    downloader::impl::parse_content_range_total(http::fields const& headers)
    {
        auto cr = headers[http::field::content_range];
        if (cr.empty())
        {
            return 0;
        }
        auto s = std::string_view(cr);
        auto slash = s.rfind('/');
        if (slash == std::string_view::npos)
        {
            return 0;
        }
        auto total_str = s.substr(slash + 1);
        if (total_str == "*")
        {
            return 0;
        }
        try
        {
            return std::stoull(std::string(total_str));
        }
        catch (...)
        {
            return 0;
        }
    }

    std::optional<std::uint64_t>
    downloader::impl::parse_content_range_start(http::fields const& headers)
    {
        auto cr = headers[http::field::content_range];
        if (cr.empty())
        {
            return std::nullopt;
        }
        std::string_view s(cr);
        constexpr std::string_view prefix = "bytes ";
        if (!s.starts_with(prefix))
        {
            return std::nullopt;
        }
        s.remove_prefix(prefix.size());
        auto dash = s.find('-');
        if (dash == std::string_view::npos)
        {
            return std::nullopt;
        }
        try
        {
            return std::stoull(std::string(s.substr(0, dash)));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::string
    downloader::impl::parse_content_disposition_filename(http::fields const& headers)
    {
        auto cd = headers[http::field::content_disposition];
        if (cd.empty())
        {
            return {};
        }
        std::string s(cd);
        auto idx = s.find("filename*=");
        if (idx != std::string::npos)
        {
            auto start = s.find('\'', idx + 10);
            if (start != std::string::npos)
            {
                start = s.find('\'', start + 1);
                if (start != std::string::npos)
                {
                    ++start;
                    auto end = s.find(';', start);
                    auto val = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    trim(val);
                    if (!val.empty())
                    {
                        return util::url_decode(std::string_view(val));
                    }
                }
            }
        }
        idx = s.find("filename=");
        if (idx != std::string::npos)
        {
            auto start = idx + 9;
            if (start < s.size())
            {
                if (s[start] == '"')
                {
                    ++start;
                    auto end = s.find('"', start);
                    return s.substr(start, end - start);
                }
                auto end = s.find(';', start);
                auto val = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
                trim(val);
                return val;
            }
        }
        return {};
    }

    std::optional<downloader::impl::redirect_target>
    downloader::impl::parse_redirect(http::fields const& headers)
    {
        auto loc = headers[http::field::location];
        if (loc.empty())
        {
            return std::nullopt;
        }
        redirect_target t;
        std::string location(loc);
        if (location.starts_with("http://") || location.starts_with("https://"))
        {
            auto ui = parse_url(location);
            t.host = ui.host;
            t.port = ui.port;
            t.ssl = ui.ssl;
            t.path = ui.path;
        }
        else
        {
            t.path = location;
        }
        t.valid = true;
        return t;
    }

    // =========================================================================
    // construction
    // =========================================================================

    downloader::impl::impl(net::any_io_executor ex, std::shared_ptr<http_client_pool> pool)
        : executor_(ex)
        , pause_event_(ex)
        , pool_(std::move(pool))
    {
    }

    downloader::impl::~impl() { cancel(); }

    // =========================================================================
    // configuration
    // =========================================================================

    void
    downloader::impl::set_config(downloader::config const& cfg)
    {
        std::lock_guard lk(config_mutex_);
        config_ = cfg;
    }

    void
    downloader::impl::set_progress_callback(downloader::progress_callback cb)
    {
        std::lock_guard lk(callback_mutex_);
        progress_cb_ = std::move(cb);
    }

    void
    downloader::impl::set_state_callback(downloader::state_callback cb)
    {
        std::lock_guard lk(callback_mutex_);
        state_cb_ = std::move(cb);
    }

    void
    downloader::impl::set_cache(std::shared_ptr<cache> c)
    {
        cache_ = std::move(c);
    }

    std::shared_ptr<cache>
    downloader::impl::get_cache() const
    {
        return cache_;
    }

    std::shared_ptr<http_client_pool>
    downloader::impl::get_http_pool() const
    {
        return pool_;
    }

    downloader::config const&
    downloader::impl::get_config() const
    {
        return config_;
    }

    downloader::state
    downloader::impl::current_state() const
    {
        std::lock_guard lk(state_mutex_);
        return state_;
    }

    void
    downloader::impl::cancel()
    {
        cancelled_.store(true, std::memory_order_relaxed);
        pause_event_.notify_all();
    }

    void
    downloader::impl::pause()
    {
        paused_.store(true, std::memory_order_relaxed);
    }

    void
    downloader::impl::resume()
    {
        paused_.store(false, std::memory_order_relaxed);
        pause_event_.notify_all();
    }

    bool
    downloader::impl::is_paused() const
    {
        return paused_.load(std::memory_order_relaxed);
    }

    std::string
    downloader::impl::suggested_filename() const
    {
        std::lock_guard lk(filename_mutex_);
        return suggested_filename_;
    }

    // =========================================================================
    // state persistence
    // =========================================================================

    fs::path
    downloader::impl::state_path(fs::path const& save_path)
    {
        return fs::path(save_path.string() + ".dlstate");
    }

    void
    downloader::impl::save_state(fs::path const& save_path)
    {
        if (!active_config_.save_state)
        {
            return;
        }
        auto sp = state_path(save_path);
        std::ofstream f(sp, std::ios::trunc);
        if (!f.is_open())
        {
            return;
        }
        f << "url=" << state_url_ << '\n';
        f << "content_length=" << total_bytes_ << '\n';
        f << "segments=" << segments_.size() << '\n';
        for (auto const& seg : segments_)
        {
            std::error_code ec;
            std::uint64_t downloaded = 0;
            if (fs::exists(seg.part_path, ec) && !ec)
            {
                downloaded = fs::file_size(seg.part_path, ec);
                if (ec)
                {
                    downloaded = 0;
                }
            }
            f << "seg" << seg.index << "_downloaded=" << downloaded << '\n';
        }
    }

    downloader::impl::download_state
    downloader::impl::load_state(fs::path const& save_path) const
    {
        download_state st;
        auto sp = state_path(save_path);
        std::error_code ec;
        if (!fs::exists(sp, ec) || ec)
        {
            return st;
        }
        std::ifstream f(sp);
        if (!f.is_open())
        {
            return st;
        }
        std::string line;
        while (std::getline(f, line))
        {
            trim(line);
            if (line.empty())
            {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            trim(key);
            trim(val);
            if (key == "url")
            {
                st.url = val;
            }
            else if (key == "content_length")
            {
                try
                {
                    st.content_length = std::stoull(val);
                }
                catch (...)
                {
                }
            }
            else if (key == "segments")
            {
                try
                {
                    st.segments = std::stoi(val);
                }
                catch (...)
                {
                }
            }
            else if (key.starts_with("seg") && key.ends_with("_downloaded"))
            {
                try
                {
                    auto v = std::stoull(val);
                    st.seg_downloaded.push_back(v);
                }
                catch (...)
                {
                }
            }
        }
        return st;
    }

    void
    downloader::impl::del_state(fs::path const& save_path) const
    {
        auto sp = state_path(save_path);
        std::error_code ec;
        fs::remove(sp, ec);
    }

    // =========================================================================
    // state / progress
    // =========================================================================

    void
    downloader::impl::set_state(downloader::state st, boost::system::error_code ec)
    {
        downloader::state_callback cb;
        {
            std::lock_guard lk(state_mutex_);
            state_ = st;
        }
        {
            std::lock_guard lk(callback_mutex_);
            cb = state_cb_;
        }
        if (cb)
        {
            cb(st, ec);
        }
    }

    net::awaitable<boost::system::error_code>
    downloader::impl::co_wait_if_paused()
    {
        while (paused_.load(std::memory_order_relaxed))
        {
            set_state(downloader::state::paused, {});
            co_await pause_event_.wait();
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }
        }
        set_state(downloader::state::downloading, {});
        co_return boost::system::error_code {};
    }

    void
    downloader::impl::update_progress(std::uint64_t delta_bytes)
    {
        downloader::progress_callback cb;
        {
            std::lock_guard lk(callback_mutex_);
            cb = progress_cb_;
        }
        if (!cb || delta_bytes == 0)
        {
            return;
        }
        downloader::progress_info info;
        {
            std::lock_guard lk(progress_mutex_);
            downloaded_bytes_ += delta_bytes;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<double>(now - progress_start_);
            info.total_bytes = total_bytes_;
            info.downloaded_bytes = downloaded_bytes_;
            info.active_segments = active_segments_;
            info.total_segments = total_segments_;
            if (elapsed.count() > 0.0 && downloaded_bytes_ > 0)
            {
                info.speed_bytes_per_sec
                    = static_cast<std::uint64_t>(static_cast<double>(downloaded_bytes_) / elapsed.count());
                if (total_bytes_ > 0 && info.speed_bytes_per_sec > 0)
                {
                    auto remaining = total_bytes_ - downloaded_bytes_;
                    info.eta = std::chrono::seconds(static_cast<int64_t>(
                        static_cast<double>(remaining) / static_cast<double>(info.speed_bytes_per_sec)));
                }
            }
        }
        cb(info);
    }

    void
    downloader::impl::store_suggested_filename(http::fields const& headers)
    {
        auto fname = parse_content_disposition_filename(headers);
        if (fname.empty())
        {
            return;
        }
        std::lock_guard lk(filename_mutex_);
        suggested_filename_ = std::move(fname);
    }

    void
    downloader::impl::record_final_ui(url_info const& ui)
    {
        std::lock_guard lk(resource_mutex_);
        final_ui_ = ui;
        has_final_ui_ = true;
    }

    void
    downloader::impl::record_resource_headers(http::fields const& headers)
    {
        std::lock_guard lk(resource_mutex_);
        resource_headers_ = headers;
    }

    // =========================================================================
    // cache helpers
    // =========================================================================

    net::awaitable<bool>
    downloader::impl::check_remote_cache(url_info const& ui, http_meta const& meta)
    {
        if (!cache_)
        {
            co_return false;
        }
        http::fields req_headers;
        if (meta.etag.empty() && meta.last_modified.empty())
        {
            // No validator: there is no way to prove the cached body is still
            // current, so never serve it.
            co_return false;
        }
        if (!meta.etag.empty())
        {
            req_headers.set(http::field::if_none_match, meta.etag);
        }
        if (!meta.last_modified.empty())
        {
            req_headers.set(http::field::if_modified_since, meta.last_modified);
        }
        auto result = co_await send_request(ui, http::verb::head, req_headers);
        if (result.status != http::status::not_modified)
        {
            co_return false;
        }
        // Revalidate the final origin too: a URL that now redirects elsewhere
        // must not be served from an entry recorded against the old target.
        if (!meta.final_url.empty() && make_url_string(result.final_ui) != meta.final_url)
        {
            co_return false;
        }
        co_return true;
    }

    // =========================================================================
    // probe Content-Length
    // =========================================================================

    net::awaitable<downloader::impl::probe_result>
    downloader::impl::probe_content_length(url_info const& ui)
    {
        probe_result res;
        auto result = co_await send_request(ui, http::verb::head);
        if (result.status == http::status::ok)
        {
            res.headers = result.headers;
            res.content_length = parse_content_length(result.headers);
        }
        co_return res;
    }

    // =========================================================================
    // send_request (with redirect)
    // =========================================================================

    net::awaitable<downloader::impl::request_result>
    downloader::impl::send_request(url_info const& ui, http::verb method, http::fields const& req_headers)
    {
        http::fields merged = custom_headers_;
        for (auto const& f : req_headers)
        {
            merged.set(f.name_string(), f.value());
        }
        // 下载场景保存原始字节（断点续传/分片合并依赖），不接受内容编码压缩。
        merged.set(http::field::accept_encoding, "identity");

        auto h = ui.host;
        auto p = ui.port;
        auto s = ui.ssl;
        auto t = ui.path;

        for (int redir = 0; redir <= active_config_.max_redirects; ++redir)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return request_result {};
            }

            assert(pool_);
            auto handle = co_await pool_->async_acquire(h, p, s, active_config_.acquire_timeout);
            if (!handle)
            {
                co_return request_result {};
            }
            handle->set_timeout(active_config_.timeout);
            handle->set_max_redirects(0);
            handle->set_verify_ssl(active_config_.verify_ssl);
            handle->set_download_rate_limit(per_connection_rate_);

            auto req = httplib::client::request(method, t, merged);
            auto resp_result = co_await handle->async_send_request(std::move(req), http_client::body_mode::lazy);
            if (!resp_result.has_value())
            {
                co_return request_result {};
            }
            auto resp = std::move(resp_result).value();

            auto status = resp.result();

            if ((status == http::status::moved_permanently || status == http::status::found
                 || status == http::status::see_other || status == http::status::temporary_redirect
                 || status == http::status::permanent_redirect)
                && redir < active_config_.max_redirects)
            {
                auto rt = parse_redirect(resp.headers());
                if (rt.has_value() && rt->valid)
                {
                    if (!rt->host.empty())
                    {
                        // CL-02: 仅当 Location 为绝对 URL 且 origin 变化时移除敏感头，避免凭据泄露；
                        // 同 origin 重定向保留原头。
                        if (rt->host != h || rt->port != p || rt->ssl != s)
                        {
                            redirect::strip_origin_bound_headers(merged);
                        }
                        h = rt->host;
                        p = rt->port == 0 ? p : rt->port;
                        s = rt->ssl;
                        t = rt->path.empty() ? "/" : rt->path;
                    }
                    else
                    {
                        // Relative reference: resolve against the current target so
                        // Location values like "final" or "../a/b" work correctly.
                        t = redirect::resolve_redirect_target(t, rt->path);
                    }
                    continue;
                }
            }

            request_result rr;
            rr.handle = std::move(handle);
            rr.response = std::move(resp);
            rr.headers = rr.response.headers();
            rr.status = status;
            rr.final_ui = url_info { h, p, s, t };
            record_final_ui(rr.final_ui);
            co_return rr;
        }

        co_return request_result {};
    }

    // =========================================================================
    // single-segment download
    // =========================================================================

    net::awaitable<boost::system::error_code>
    downloader::impl::co_download_single(url_info const& ui, fs::path const& save_path)
    {
        for (int attempt = 0; attempt <= active_config_.max_retries; ++attempt)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }

            auto pause_ec = co_await co_wait_if_paused();
            if (pause_ec)
            {
                co_return pause_ec;
            }

            std::uint64_t existing_size = 0;
            http::fields req_headers;

            if (active_config_.resume && attempt == 0)
            {
                std::error_code ec;
                if (fs::exists(save_path, ec) && !ec)
                {
                    existing_size = fs::file_size(save_path, ec);
                    if (ec)
                    {
                        existing_size = 0;
                    }
                }
            }

            if (existing_size > 0)
            {
                req_headers.set(http::field::range, std::format("bytes={}-", existing_size));
            }

            auto result = co_await send_request(ui, http::verb::get, req_headers);
            if (!result.handle)
            {
                if (attempt == active_config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                continue;
            }

            auto status = result.status;
            if (status != http::status::ok && status != http::status::partial_content)
            {
                if (attempt == active_config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                if (!active_config_.resume)
                {
                    std::error_code ec;
                    fs::remove(save_path, ec);
                }
                continue;
            }

            auto content_length = parse_content_length(result.headers);
            auto content_range_total = parse_content_range_total(result.headers);
            if (status == http::status::ok && existing_size > 0)
            {
                existing_size = 0;
            }
            else if (status == http::status::partial_content)
            {
                // Guard against a server that returns 206 with an unexpected
                // starting offset, which would corrupt the appended data.
                if (auto start = parse_content_range_start(result.headers);
                    start.has_value() && *start != existing_size)
                {
                    if (attempt == active_config_.max_retries)
                    {
                        co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                    }
                    std::error_code ec;
                    fs::remove(save_path, ec);
                    existing_size = 0;
                    continue;
                }
            }
            auto file_total = content_range_total > 0 ? content_range_total : (content_length + existing_size);

            {
                std::lock_guard lk(progress_mutex_);
                total_bytes_ = file_total;
                downloaded_bytes_ = existing_size;
                progress_start_ = std::chrono::steady_clock::now();
                total_segments_ = 1;
                active_segments_ = 1;
            }

            auto open_mode = std::ios::out | std::ios::binary;
            if (existing_size > 0)
            {
                open_mode |= std::ios::app;
            }
            else
            {
                open_mode |= std::ios::trunc;
            }

            std::ofstream out(save_path, open_mode);
            if (!out.is_open())
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }

            auto& resp = result.response;
            std::vector<char> buf(kReadBufSize);

            while (!resp.is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_return boost::asio::error::operation_aborted;
                }

                auto pause_ec = co_await co_wait_if_paused();
                if (pause_ec)
                {
                    co_return pause_ec;
                }

                auto r = co_await resp.read_some_decompressed(net::buffer(buf));
                if (r.has_error())
                {
                    co_return r.error();
                }
                auto n = r.value();
                if (n == 0)
                {
                    break;
                }

                out.write(buf.data(), n);
                if (!out)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::no_space_on_device);
                }

                update_progress(n);
            }

            out.close();

            store_suggested_filename(result.headers);
            record_resource_headers(result.headers);

            {
                downloader::progress_callback cb;
                {
                    std::lock_guard lk(callback_mutex_);
                    cb = progress_cb_;
                }
                if (cb)
                {
                    auto final_sz = file_total > 0 ? file_total : existing_size;
                    cb(downloader::progress_info { final_sz, final_sz, 0, std::chrono::seconds(0), 0, 1 });
                }
            }

            co_return boost::system::error_code {};
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
    }

    // =========================================================================
    // segment download
    // =========================================================================

    net::awaitable<boost::system::error_code>
    downloader::impl::co_download_segment(url_info const& ui,
                                          std::uint64_t start,
                                          std::uint64_t end,
                                          fs::path const& part_path)
    {
        for (int attempt = 0; attempt <= active_config_.max_retries; ++attempt)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }

            auto pause_ec = co_await co_wait_if_paused();
            if (pause_ec)
            {
                co_return pause_ec;
            }

            std::uint64_t resume_at = start;
            std::ios_base::openmode open_mode = std::ios::out | std::ios::binary;

            if (active_config_.resume)
            {
                std::error_code ec;
                if (fs::exists(part_path, ec) && !ec)
                {
                    auto sz = fs::file_size(part_path, ec);
                    if (!ec && sz > 0)
                    {
                        auto seg_len = end - start + 1;
                        if (sz >= seg_len)
                        {
                            // Already fully downloaded in a previous run.
                            co_return boost::system::error_code {};
                        }
                        resume_at = start + sz;
                    }
                }
            }

            if (resume_at == start)
            {
                open_mode |= std::ios::trunc;
            }
            else
            {
                open_mode |= std::ios::app;
            }

            http::fields req_headers;
            req_headers.set(http::field::range, std::format("bytes={}-{}", resume_at, end));

            auto result = co_await send_request(ui, http::verb::get, req_headers);
            if (!result.handle)
            {
                if (attempt == active_config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                continue;
            }

            if (result.status == http::status::ok)
            {
                // The server ignored the Range request: byte ranges are not
                // supported, so segmented downloading cannot proceed. Signal the
                // caller to fall back to a single-segment download.
                co_return boost::system::errc::make_error_code(boost::system::errc::operation_not_supported);
            }
            if (result.status != http::status::partial_content)
            {
                if (attempt == active_config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                continue;
            }

            // Guard against a server returning a range that does not start where
            // we asked; appending it would silently corrupt the merged output.
            if (auto range_start = parse_content_range_start(result.headers);
                range_start.has_value() && *range_start != resume_at)
            {
                if (attempt == active_config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                std::error_code rm_ec;
                fs::remove(part_path, rm_ec);
                continue;
            }

            store_suggested_filename(result.headers);
            record_resource_headers(result.headers);

            std::ofstream out(part_path, open_mode);
            if (!out.is_open())
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }

            auto& resp = result.response;
            std::vector<char> buf(kReadBufSize);

            while (!resp.is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_return boost::asio::error::operation_aborted;
                }

                auto pause_ec = co_await co_wait_if_paused();
                if (pause_ec)
                {
                    co_return pause_ec;
                }

                auto r = co_await resp.read_some_decompressed(net::buffer(buf));
                if (r.has_error())
                {
                    co_return r.error();
                }
                auto n = r.value();
                if (n == 0)
                {
                    break;
                }

                out.write(buf.data(), n);
                if (!out)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::no_space_on_device);
                }

                update_progress(n);
            }
            out.close();

            co_return boost::system::error_code {};
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
    }

    // =========================================================================
    // multi-segment orchestration
    // =========================================================================

    net::awaitable<boost::system::error_code>
    downloader::impl::co_download_multi_segment(url_info const& ui,
                                                fs::path const& save_path,
                                                std::uint64_t content_length,
                                                http::fields const& probe_headers)
    {
        int seg_count = active_config_.segments;
        if (seg_count < 2)
        {
            seg_count = 2;
        }
        if (seg_count > 32)
        {
            seg_count = 32;
        }

        auto seg_size = content_length / seg_count;
        auto remainder = content_length % seg_count;

        if (seg_size == 0 && content_length > 0)
        {
            seg_count = static_cast<int>(content_length);
            seg_size = 1;
            remainder = 0;
        }

        segments_.clear();
        segments_.reserve(seg_count);

        std::uint64_t offset = 0;
        for (int i = 0; i < seg_count; ++i)
        {
            auto sz = seg_size + (static_cast<std::uint64_t>(i) < remainder ? 1 : 0);
            std::uint64_t start_byte = offset;
            std::uint64_t end_byte = offset + sz - 1;
            offset += sz;

            auto part_path = fs::path(save_path.string() + ".part" + std::to_string(i));
            segments_.push_back({ start_byte, end_byte, i, part_path });
        }

        // Decide whether previously written part files can be reused. A sidecar
        // state file pins down the layout (url + content length + segment count)
        // so a stale state from an unrelated download is never resumed.
        bool can_resume = false;
        if (active_config_.resume && active_config_.save_state)
        {
            auto prev = load_state(save_path);
            if (prev.content_length == content_length && prev.segments == seg_count && prev.url == state_url_)
            {
                can_resume = true;
            }
        }

        // Drop part files that belong to an incompatible or unknown layout.
        {
            auto stem = save_path.filename().string();
            std::error_code ec;
            for (auto const& de : fs::directory_iterator(save_path.parent_path(), ec))
            {
                if (ec)
                {
                    break;
                }
                auto name = de.path().filename().string();
                if (!name.starts_with(stem + ".part"))
                {
                    continue;
                }
                auto idx_str = name.substr(stem.size() + 5); // skip "<stem>.part"
                int idx = -1;
                try
                {
                    idx = std::stoi(idx_str);
                }
                catch (...)
                {
                    idx = -1;
                }
                if (!can_resume || idx < 0 || idx >= seg_count)
                {
                    std::error_code rm_ec;
                    fs::remove(de.path(), rm_ec);
                }
            }
        }
        if (!can_resume)
        {
            del_state(save_path);
        }

        // Progress must be seeded from the bytes actually present on disk, not
        // from the state file, otherwise resumed segments are double-counted.
        std::uint64_t already_downloaded = 0;
        if (can_resume)
        {
            for (auto const& seg : segments_)
            {
                std::error_code ec;
                auto sz = fs::file_size(seg.part_path, ec);
                if (!ec && sz > 0)
                {
                    already_downloaded += std::min<std::uint64_t>(sz, seg.end_byte - seg.start_byte + 1);
                }
            }
        }

        {
            std::lock_guard lk(progress_mutex_);
            total_bytes_ = content_length;
            downloaded_bytes_ = already_downloaded;
            total_segments_ = seg_count;
            active_segments_ = seg_count;
            progress_start_ = std::chrono::steady_clock::now();
        }

        store_suggested_filename(probe_headers);

        // Persist the layout up-front so an interrupted (e.g. crashed) run can be
        // resumed on the next attempt.
        save_state(save_path);

        {
            downloader::progress_callback cb;
            {
                std::lock_guard lk(callback_mutex_);
                cb = progress_cb_;
            }
            if (cb)
            {
                cb(downloader::progress_info { content_length,
                                               already_downloaded,
                                               0,
                                               std::chrono::seconds(0),
                                               seg_count,
                                               seg_count });
            }
        }

        std::vector<net::awaitable<boost::system::error_code>> ops;
        ops.reserve(seg_count);

        for (auto& seg : segments_)
        {
            ops.emplace_back(co_download_segment(ui, seg.start_byte, seg.end_byte, seg.part_path));
        }

        auto remove_all_parts = [&]
        {
            for (auto& s : segments_)
            {
                std::error_code fs_ec;
                fs::remove(s.part_path, fs_ec);
            }
        };

        boost::system::error_code first_error;
        boost::system::error_code aborted_error;
        bool ranges_unsupported = false;

        try
        {
            auto errors = co_await util::when_all(std::move(ops));
            {
                std::lock_guard lk(progress_mutex_);
                active_segments_ = 0;
            }
            for (auto const& ec : errors)
            {
                if (!ec)
                {
                    continue;
                }
                if (ec == boost::system::errc::make_error_code(boost::system::errc::operation_not_supported))
                {
                    ranges_unsupported = true;
                }
                else if (ec == boost::asio::error::operation_aborted)
                {
                    if (!aborted_error)
                    {
                        aborted_error = ec;
                    }
                }
                else if (!first_error)
                {
                    first_error = ec;
                }
            }
        }
        catch (...)
        {
            first_error = boost::system::errc::make_error_code(boost::system::errc::io_error);
        }

        if (first_error)
        {
            del_state(save_path);
            remove_all_parts();
            co_return first_error;
        }

        if (ranges_unsupported)
        {
            // Byte ranges are unavailable; leave no partial artifacts behind and
            // let the caller retry as a single stream.
            del_state(save_path);
            remove_all_parts();
            co_return boost::system::errc::make_error_code(boost::system::errc::operation_not_supported);
        }

        if (aborted_error)
        {
            // Keep parts + state so a later run can resume where it stopped.
            save_state(save_path);
            co_return aborted_error;
        }

        set_state(downloader::state::merging, {});
        auto merge_ec = merge_parts_sync(save_path, seg_count);
        if (merge_ec)
        {
            co_return merge_ec;
        }

        del_state(save_path);

        co_return boost::system::error_code {};
    }

    // =========================================================================
    // merge
    // =========================================================================

    boost::system::error_code
    downloader::impl::merge_parts_sync(fs::path const& save_path, int total_segments)
    {
        std::ofstream out(save_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
        }

        constexpr std::size_t kBufSize = 1024 * 1024;
        auto buf = std::make_unique<char[]>(kBufSize);

        for (int i = 0; i < total_segments; ++i)
        {
            auto part_path = fs::path(save_path.string() + ".part" + std::to_string(i));
            std::ifstream in(part_path, std::ios::binary);
            if (!in.is_open())
            {
                out.close();
                std::error_code rm_ec;
                fs::remove(save_path, rm_ec);
                return boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            }

            while (in)
            {
                in.read(buf.get(), kBufSize);
                auto n = static_cast<std::size_t>(in.gcount());
                if (n == 0)
                {
                    break;
                }
                out.write(buf.get(), n);
                if (!out)
                {
                    out.close();
                    std::error_code rm_ec;
                    fs::remove(save_path, rm_ec);
                    return boost::system::errc::make_error_code(boost::system::errc::no_space_on_device);
                }
            }
            in.close();
        }
        out.close();

        for (int i = 0; i < total_segments; ++i)
        {
            auto part_path = fs::path(save_path.string() + ".part" + std::to_string(i));
            std::error_code ec;
            fs::remove(part_path, ec);
        }

        return {};
    }

    // =========================================================================
    // main entry
    // =========================================================================

    net::awaitable<boost::system::error_code>
    downloader::impl::async_download(std::string_view url, fs::path const& save_path, http::fields const& headers)
    {
        if (cancelled_.exchange(false, std::memory_order_relaxed))
        {
            auto ec = boost::asio::error::operation_aborted;
            set_state(downloader::state::cancelled, ec);
            co_return ec;
        }

        {
            std::lock_guard lk(config_mutex_);
            active_config_ = config_;
        }
        per_connection_rate_ = active_config_.max_speed_bytes_per_sec;
        custom_headers_ = headers;
        auth_scope_ = cache_auth_scope(custom_headers_);
        {
            std::lock_guard lk(resource_mutex_);
            resource_headers_.clear();
            final_ui_ = {};
            has_final_ui_ = false;
        }

        url_info ui;
        try
        {
            ui = parse_url(url);
        }
        catch (std::invalid_argument const&)
        {
            auto ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            set_state(downloader::state::failed, ec);
            co_return ec;
        }

        state_url_ = make_cache_key(ui);

        set_state(downloader::state::connecting, {});

        boost::system::error_code ec;
        try
        {
            if (cache_)
            {
                auto entry = cache_->get(state_url_);
                if (entry.has_value())
                {
                    auto meta = parse_http_meta(entry->metadata);
                    if (meta.has_value())
                    {
                        set_state(downloader::state::downloading, {});
                        bool usable = false;
                        if (!meta->must_revalidate && meta->fresh_until.has_value()
                            && std::chrono::system_clock::now() < *meta->fresh_until)
                        {
                            // Still fresh: serve the cached body without any
                            // network round-trip.
                            usable = true;
                        }
                        else
                        {
                            usable = co_await check_remote_cache(ui, *meta);
                        }
                        if (usable)
                        {
                            // Re-fetch immediately before copying so a concurrent
                            // cleanup cannot evict the entry between validation
                            // and use (narrowing the TOCTOU window).
                            auto fresh = cache_->get(state_url_);
                            if (fresh.has_value())
                            {
                                std::error_code copy_ec;
                                fs::copy_file(fresh->body_path,
                                              save_path,
                                              fs::copy_options::overwrite_existing,
                                              copy_ec);
                                if (!copy_ec)
                                {
                                    set_state(downloader::state::completed, {});
                                    co_return boost::system::error_code {};
                                }
                            }
                        }
                    }
                }
            }

            auto probe = co_await probe_content_length(ui);
            auto content_length = probe.content_length;

            store_suggested_filename(probe.headers);

            set_state(downloader::state::downloading, {});

            if (content_length > 0 && active_config_.segments > 1)
            {
                // Spread the configured cap over the concurrent segment
                // connections so the aggregate stays near the requested rate.
                if (per_connection_rate_ > 0)
                {
                    auto segs = static_cast<std::uint64_t>(std::clamp(active_config_.segments, 2, 32));
                    per_connection_rate_ = std::max<std::uint64_t>(1, per_connection_rate_ / segs);
                }
                ec = co_await co_download_multi_segment(ui, save_path, content_length, probe.headers);
                if (ec == boost::system::errc::make_error_code(boost::system::errc::operation_not_supported))
                {
                    // Server does not honor Range requests; fall back to a plain
                    // single-stream download.
                    per_connection_rate_ = active_config_.max_speed_bytes_per_sec;
                    ec = co_await co_download_single(ui, save_path);
                }
            }
            else
            {
                ec = co_await co_download_single(ui, save_path);
            }

            if (!ec && cache_ && !save_path.empty())
            {
                http::fields response_headers;
                url_info final_ui;
                bool has_final = false;
                {
                    std::lock_guard lk(resource_mutex_);
                    response_headers = resource_headers_;
                    final_ui = final_ui_;
                    has_final = has_final_ui_;
                }
                bool cacheable = response_is_cacheable(probe.headers);
                if (response_headers.begin() != response_headers.end())
                {
                    cacheable = cacheable && response_is_cacheable(response_headers);
                }
                if (cacheable)
                {
                    auto meta = make_http_meta(response_headers, probe.headers, final_ui, has_final);
                    // Retention is governed by the cache's max_age (so stale
                    // entries stay available for revalidation); HTTP freshness
                    // travels inside the opaque metadata blob.
                    cache_->put(state_url_, save_path, serialize_http_meta(meta), std::nullopt);
                }
            }
        }
        catch (std::exception const&)
        {
            ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
        }
        catch (...)
        {
            ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
        }

        if (ec)
        {
            if (ec == boost::asio::error::operation_aborted)
            {
                set_state(downloader::state::cancelled, ec);
            }
            else
            {
                set_state(downloader::state::failed, ec);
            }
            co_return ec;
        }

        set_state(downloader::state::completed, {});
        co_return boost::system::error_code {};
    }

    // =========================================================================
    // downloader public API
    // =========================================================================

    downloader::downloader(net::io_context& ex, std::shared_ptr<http_client_pool> pool)
        : impl_(std::make_unique<impl>(ex.get_executor(), std::move(pool)))
    {
    }

    downloader::downloader(net::any_io_executor const& ex, std::shared_ptr<http_client_pool> pool)
        : impl_(std::make_unique<impl>(ex, std::move(pool)))
    {
    }

    downloader::downloader(downloader&&) noexcept = default;

    downloader& downloader::operator=(downloader&&) noexcept = default;

    downloader::~downloader() {}

    void
    downloader::set_config(config const& cfg)
    {
        impl_->set_config(cfg);
    }

    void
    downloader::set_progress_callback(progress_callback cb)
    {
        impl_->set_progress_callback(std::move(cb));
    }

    void
    downloader::set_state_callback(state_callback cb)
    {
        impl_->set_state_callback(std::move(cb));
    }

    void
    downloader::set_cache(std::shared_ptr<cache> c)
    {
        impl_->set_cache(std::move(c));
    }

    std::shared_ptr<cache>
    downloader::get_cache() const
    {
        return impl_->get_cache();
    }

    std::shared_ptr<http_client_pool>
    downloader::get_http_pool() const
    {
        return impl_->get_http_pool();
    }

    downloader::config const&
    downloader::get_config() const
    {
        return impl_->get_config();
    }

    net::awaitable<boost::system::error_code>
    downloader::async_download(std::string_view url, fs::path const& save_path, http::fields const& headers)
    {
        co_return co_await impl_->async_download(url, save_path, headers);
    }

    boost::system::error_code
    downloader::download(std::string_view url, fs::path const& save_path, http::fields const& headers)
    {
        auto future = net::co_spawn(impl_->executor_, impl_->async_download(url, save_path, headers), net::use_future);
        return future.get();
    }

    void
    downloader::cancel()
    {
        impl_->cancel();
    }

    void
    downloader::pause()
    {
        impl_->pause();
    }

    void
    downloader::resume()
    {
        impl_->resume();
    }

    bool
    downloader::is_paused() const
    {
        return impl_->is_paused();
    }

    std::string
    downloader::suggested_filename() const
    {
        return impl_->suggested_filename();
    }

    downloader::state
    downloader::current_state() const
    {
        return impl_->current_state();
    }

} // namespace httplib::client
