#include "downloader_impl.h"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/client/write_session.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/when_all.hpp"
#include <boost/url.hpp>
#include <format>
#include <fstream>
#include <stdexcept>

namespace httplib::client
{

    namespace
    {
        constexpr std::size_t kReadBufSize = 64 * 1024;

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
        : executor_(std::move(ex))
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
        if (!config_.save_state)
        {
            return;
        }
        auto sp = state_path(save_path);
        std::ofstream f(sp, std::ios::trunc);
        if (!f.is_open())
        {
            return;
        }
        f << "url=current\n";
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
    downloader::impl::set_state(downloader::state st, std::string_view msg)
    {
        downloader::state_callback cb;
        {
            std::lock_guard lk(state_mutex_);
            state_ = st;
            state_msg_ = msg;
        }
        {
            std::lock_guard lk(callback_mutex_);
            cb = state_cb_;
        }
        if (cb)
        {
            cb(st, msg);
        }
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

    // =========================================================================
    // cache helpers
    // =========================================================================

    net::awaitable<bool>
    downloader::impl::check_remote_cache(url_info const& ui)
    {
        if (!cache_)
        {
            co_return false;
        }
        auto entry = cache_->get(ui.path);
        if (!entry.has_value())
        {
            co_return false;
        }
        auto& cached = entry.value();
        http::fields req_headers;
        auto etag = cached.headers[http::field::etag];
        auto last_mod = cached.headers[http::field::last_modified];
        if (!etag.empty())
        {
            req_headers.set(http::field::if_none_match, etag);
        }
        if (!last_mod.empty())
        {
            req_headers.set(http::field::if_modified_since, last_mod);
        }
        auto result = co_await send_request(ui, http::verb::get, req_headers);
        co_return result.status == http::status::not_modified;
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

        auto h = ui.host;
        auto p = ui.port;
        auto s = ui.ssl;
        auto t = ui.path;

        for (int redir = 0; redir <= config_.max_redirects; ++redir)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return request_result {};
            }

            assert(pool_);
            auto handle = co_await pool_->async_acquire(h, p, s, config_.acquire_timeout);
            if (!handle)
            {
                co_return request_result {};
            }
            handle->set_timeout(config_.timeout);
            handle->set_max_redirects(0);
            handle->set_verify_ssl(config_.verify_ssl);

            auto writer = handle->create_writer();
            auto ec = co_await writer->write_header(method, t, merged);
            if (ec)
            {
                co_return request_result {};
            }
            writer.reset();

            auto reader = handle->create_reader();
            ec = co_await reader->read_header();
            if (ec)
            {
                co_return request_result {};
            }

            auto status = reader->result();

            if ((status == http::status::moved_permanently || status == http::status::found
                 || status == http::status::see_other || status == http::status::temporary_redirect
                 || status == http::status::permanent_redirect)
                && redir < config_.max_redirects)
            {
                auto rt = parse_redirect(reader->headers());
                if (rt.has_value() && rt->valid)
                {
                    h = rt->host.empty() ? h : rt->host;
                    p = rt->port == 0 ? p : rt->port;
                    s = rt->ssl;
                    t = rt->path;
                    continue;
                }
            }

            request_result rr;
            rr.handle = std::move(handle);
            rr.reader = std::move(reader);
            rr.headers = rr.reader->headers();
            rr.status = status;
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
        for (int attempt = 0; attempt <= config_.max_retries; ++attempt)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }

            std::uint64_t existing_size = 0;
            http::fields req_headers;

            if (config_.resume && attempt == 0)
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
                if (attempt == config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                continue;
            }

            auto status = result.status;
            if (status != http::status::ok && status != http::status::partial_content)
            {
                if (attempt == config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                if (!config_.resume)
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

            auto reader = result.reader;
            std::vector<char> buf(kReadBufSize);

            while (!reader->is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_return boost::asio::error::operation_aborted;
                }

                auto r = co_await reader->read_body(net::buffer(buf));
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

            {
                auto fname = parse_content_disposition_filename(result.headers);
                if (!fname.empty())
                {
                    std::lock_guard lk(filename_mutex_);
                    suggested_filename_ = std::move(fname);
                }
            }

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

            if (cache_)
            {
                cache_->put(ui.path, result.headers, save_path);
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
        for (int attempt = 0; attempt <= config_.max_retries; ++attempt)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }

            std::uint64_t resume_at = start;
            std::ios_base::openmode open_mode = std::ios::out | std::ios::binary;

            if (config_.resume)
            {
                std::error_code ec;
                if (fs::exists(part_path, ec) && !ec)
                {
                    auto sz = fs::file_size(part_path, ec);
                    if (!ec && sz > 0)
                    {
                        resume_at = start + sz;
                        if (resume_at > end)
                        {
                            co_return boost::system::error_code {};
                        }
                        open_mode |= std::ios::app;
                    }
                }
            }

            if (resume_at == start)
            {
                open_mode |= std::ios::trunc;
            }

            http::fields req_headers;
            req_headers.set(http::field::range, std::format("bytes={}-{}", resume_at, end));

            auto result = co_await send_request(ui, http::verb::get, req_headers);
            if (!result.handle)
            {
                if (attempt == config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                continue;
            }

            if (result.status != http::status::partial_content)
            {
                if (attempt == config_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                continue;
            }

            std::ofstream out(part_path, open_mode);
            if (!out.is_open())
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }

            auto reader = result.reader;
            std::vector<char> buf(kReadBufSize);

            while (!reader->is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_return boost::asio::error::operation_aborted;
                }

                auto r = co_await reader->read_body(net::buffer(buf));
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
        int seg_count = config_.segments;
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

        {
            std::error_code ec;
            for (auto const& de : fs::directory_iterator(save_path.parent_path(), ec))
            {
                if (ec)
                {
                    break;
                }
                auto stem = save_path.filename().string();
                auto name = de.path().filename().string();
                if (name.starts_with(stem + ".part"))
                {
                    fs::remove(de.path(), ec);
                }
            }
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

        std::uint64_t already_downloaded = 0;
        if (config_.resume && config_.save_state)
        {
            auto prev = load_state(save_path);
            if (prev.content_length == content_length && prev.segments == seg_count
                && prev.seg_downloaded.size() == static_cast<std::size_t>(seg_count))
            {
                for (int i = 0; i < seg_count; ++i)
                {
                    already_downloaded += prev.seg_downloaded[i];
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

        std::vector<net::awaitable<void>> ops;
        ops.reserve(seg_count);
        std::vector<boost::system::error_code> errors(seg_count);

        for (auto& seg : segments_)
        {
            ops.push_back(
                [this, &ui, &seg, &errors]() -> net::awaitable<void>
                {
                    errors[seg.index] = co_await co_download_segment(ui, seg.start_byte, seg.end_byte, seg.part_path);
                }());
        }

        co_await util::when_all(std::move(ops));

        {
            std::lock_guard lk(progress_mutex_);
            active_segments_ = 0;
        }

        save_state(save_path);

        for (auto& ec : errors)
        {
            if (ec)
            {
                del_state(save_path);
                for (auto& s : segments_)
                {
                    std::error_code fs_ec;
                    fs::remove(s.part_path, fs_ec);
                }
                co_return ec;
            }
        }

        set_state(downloader::state::merging);
        auto merge_ec = merge_parts_sync(save_path, seg_count);
        if (merge_ec)
        {
            co_return merge_ec;
        }

        del_state(save_path);

        if (cache_)
        {
            cache_->put(ui.path, probe_headers, save_path);
        }

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
            set_state(downloader::state::cancelled);
            co_return boost::asio::error::operation_aborted;
        }
        custom_headers_ = headers;
        auto ui = parse_url(url);

        set_state(downloader::state::connecting);

        if (cache_)
        {
            auto entry = cache_->get(ui.path);
            if (entry.has_value())
            {
                set_state(downloader::state::downloading);
                if (co_await check_remote_cache(ui))
                {
                    std::error_code ec;
                    fs::copy_file(entry->body_path, save_path, fs::copy_options::overwrite_existing, ec);
                    if (!ec)
                    {
                        set_state(downloader::state::completed);
                        co_return boost::system::error_code {};
                    }
                }
            }
        }

        auto probe = co_await probe_content_length(ui);
        auto content_length = probe.content_length;

        set_state(downloader::state::downloading);

        boost::system::error_code ec;

        if (content_length > 0 && config_.segments > 1)
        {
            ec = co_await co_download_multi_segment(ui, save_path, content_length, probe.headers);
        }
        else
        {
            ec = co_await co_download_single(ui, save_path);
        }

        if (ec)
        {
            set_state(downloader::state::failed, ec.message());
            co_return ec;
        }

        set_state(downloader::state::completed);
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
