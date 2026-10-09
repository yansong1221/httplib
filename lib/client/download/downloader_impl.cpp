#include "downloader_impl.h"
#include "beast_alias.hpp"
#include "client/redirect_util.hpp"
#include "http_header_util.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/url/url.hpp"
#include "httplib/util/sleep.hpp"
#include "progress_tracker.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <cassert>
#include <format>
#include <span>
#include <vector>

namespace httplib::client
{
    namespace
    {
        constexpr std::size_t kReadBufSize = 64 * 1024;
    } // namespace

    // =========================================================================
    // construction
    // =========================================================================

    downloader::impl::impl(net::any_io_executor ex, std::shared_ptr<http_client_pool> pool)
        : executor_(ex)
        , disk_(ex)
        , pool_(std::move(pool))
        , progress_(ex)
    {
    }

    downloader::impl::~impl() { cancel(); }

    // =========================================================================
    // configuration
    // =========================================================================

    void
    downloader::impl::set_config(downloader::config const& cfg)
    {
        config_.store(cfg);
    }

    void
    downloader::impl::set_progress_callback(downloader::progress_callback cb)
    {
        progress_.set_progress_callback(std::move(cb));
    }

    void
    downloader::impl::set_state_callback(downloader::state_callback cb)
    {
        progress_.set_state_callback(std::move(cb));
    }

    void
    downloader::impl::set_cache(std::shared_ptr<cache> c)
    {
        cache_manager_.set_cache(std::move(c));
    }

    std::shared_ptr<cache>
    downloader::impl::get_cache() const
    {
        return cache_manager_.raw_cache();
    }

    std::shared_ptr<http_client_pool>
    downloader::impl::get_http_pool() const
    {
        return pool_;
    }

    downloader::config
    downloader::impl::get_config() const
    {
        return config_.load();
    }

    downloader::state
    downloader::impl::current_state() const
    {
        return progress_.state();
    }

    void
    downloader::impl::cancel()
    {
        cancelled_.store(true, std::memory_order_relaxed);

        std::shared_ptr<net::cancellation_signal> signal = cancel_signal_.load();
        if (signal)
        {
            signal->emit(net::cancellation_type::all);
        }
        progress_.notify_all();
    }

    void
    downloader::impl::pause()
    {
        progress_.pause();
    }

    void
    downloader::impl::resume()
    {
        progress_.resume();
    }

    bool
    downloader::impl::is_paused() const
    {
        return progress_.is_paused();
    }

    std::string
    downloader::impl::suggested_filename() const
    {
        std::lock_guard lk(filename_mutex_);
        return suggested_filename_;
    }

    void
    downloader::impl::store_suggested_filename(httplib::headers const& headers)
    {
        auto fname = http_header_util::parse_content_disposition_filename(headers);
        if (fname.empty())
        {
            return;
        }
        std::lock_guard lk(filename_mutex_);
        suggested_filename_ = std::move(fname);
    }

    // =========================================================================
    // cache helpers
    // =========================================================================

    net::awaitable<bool>
    downloader::impl::check_remote_cache(url::url_info const& ui,
                                         cache_manager::http_meta const& meta,
                                         httplib::headers const& base_headers)
    {
        if (!cache_manager_.enabled())
        {
            co_return false;
        }
        if (meta.etag.empty() && meta.last_modified.empty())
        {
            // No validator: there is no way to prove the cached body is still
            // current, so never serve it.
            co_return false;
        }
        httplib::headers req_headers = base_headers;
        if (!meta.etag.empty())
        {
            req_headers.set(field::if_none_match, meta.etag);
        }
        if (!meta.last_modified.empty())
        {
            req_headers.set(field::if_modified_since, meta.last_modified);
        }
        auto result = co_await send_request(ui, method::head, std::move(req_headers));
        if (!result.has_value() || result->response.result() != status::not_modified)
        {
            co_return false;
        }
        // Revalidate the final origin too: a URL that now redirects elsewhere
        // must not be served from an entry recorded against the old target.
        if (!meta.final_url.empty() && result->final_ui.to_url() != meta.final_url)
        {
            co_return false;
        }
        co_return true;
    }

    // =========================================================================
    // serve from cache
    // =========================================================================

    net::awaitable<bool>
    downloader::impl::try_serve_from_cache(url::url_info const& ui,
                                           std::string const& state_url,
                                           fs::path const& save_path,
                                           httplib::headers const& headers)
    {
        if (!cache_manager_.enabled())
        {
            co_return false;
        }
        auto entry = cache_manager_.get(state_url);
        if (!entry.has_value())
        {
            co_return false;
        }
        auto meta = cache_manager::parse_meta(entry->metadata);
        if (!meta.has_value())
        {
            co_return false;
        }

        progress_.set_state(downloader::state::downloading, {});

        bool usable = false;
        if (!meta->must_revalidate && meta->fresh_until.has_value()
            && std::chrono::system_clock::now() < *meta->fresh_until)
        {
            // Still fresh: serve the cached body without any network round-trip.
            usable = true;
        }
        else
        {
            usable = co_await check_remote_cache(ui, *meta, headers);
        }
        if (!usable)
        {
            co_return false;
        }

        // Re-fetch immediately before copying so a concurrent cleanup cannot
        // evict the entry between validation and use (narrowing the TOCTOU window).
        auto fresh = cache_manager_.get(state_url);
        if (!fresh.has_value())
        {
            co_return false;
        }
        std::error_code copy_ec;
        fs::copy_file(fresh->body_path, save_path, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec)
        {
            co_return false;
        }
        progress_.finish_with_bytes(fresh->body_size);
        progress_.set_state(downloader::state::completed, {});
        co_return true;
    }

    // =========================================================================
    // send_request (with redirect)
    // =========================================================================

    net::awaitable<boost::system::result<downloader::impl::send_result>>
    downloader::impl::send_request(url::url_info const& ui, httplib::method m, httplib::headers req_headers)
    {
        downloader::config const cfg = config_.load();

        // 下载场景保存原始字节（断点续传依赖），不接受内容编码压缩。
        req_headers.set(field::accept_encoding, "identity");

        auto h = ui.host;
        auto p = ui.effective_port();
        auto s = ui.transport();
        auto t = ui.target(false);
        if (t.empty())
        {
            t = "/";
        }

        for (int redir = 0; redir <= cfg.max_redirects; ++redir)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }

            assert(pool_);
            auto handle = co_await pool_->async_acquire(h, p, s, cfg.acquire_timeout);
            if (!handle)
            {
                co_return handle.error();
            }
            handle->set_timeout(cfg.timeout);
            handle->set_max_redirects(0);
            handle->set_verify_ssl(cfg.verify_ssl);
            handle->set_download_rate_limit(cfg.max_speed_bytes_per_sec);

            auto req = httplib::client::request(m, t, req_headers);

            auto resp_result = co_await handle->async_send_request(req, http_client::body_mode::lazy);
            if (!resp_result.has_value())
            {
                co_return resp_result.error();
            }
            auto resp = std::move(resp_result).value();

            auto status = resp.result();

            if ((status == status::moved_permanently || status == status::found || status == status::see_other
                 || status == status::temporary_redirect || status == status::permanent_redirect)
                && redir < cfg.max_redirects)
            {
                auto rt = http_header_util::parse_redirect(resp.headers());
                if (rt.has_value())
                {
                    // Drain the redirect body before the handle returns to the
                    // pool; otherwise the leftover bytes corrupt the next
                    // request that reuses this connection. A failed drain means
                    // the connection is unusable, so close it explicitly.
                    if (auto drain_ec = co_await resp.read_body(); drain_ec)
                    {
                        handle->close();
                    }

                    if (!rt->host.empty())
                    {
                        // CL-02: 仅当 Location 为绝对 URL 且 origin 变化时移除敏感头，避免凭据泄露；
                        // 同 origin 重定向保留原头。
                        if (rt->host != h || rt->port != p || rt->transport() != s)
                        {
                            redirect::strip_origin_bound_headers(req_headers);
                        }
                        h = rt->host;
                        p = rt->port == 0 ? p : rt->port;
                        s = rt->transport();
                        t = rt->path.empty() ? "/" : rt->path;
                    }
                    else
                    {
                        // Relative reference: resolve against the current target so
                        // Location values like "final" or "../a/b" work correctly.
                        t = url::resolve(t, rt->path);
                    }
                    continue;
                }
            }

            co_return send_result {
                std::move(handle),
                std::move(resp),
                url::url_info { std::string(url::to_string(s)), h, p, t, {}, {} }
            };
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
    }

    // =========================================================================
    // main entry
    // =========================================================================

    net::awaitable<boost::system::error_code>
    downloader::impl::async_download(std::string_view url, fs::path const& save_path, httplib::headers const& headers)
    {
        // Install this run's own cancellation source before spawning the body,
        // so cancel() reaches it from the first moment the run is underway.
        auto signal = std::make_shared<net::cancellation_signal>();
        cancel_signal_.store(signal);

        // A prior cancel()/permanent failure only terminates the run it
        // interrupted. A fresh run starts from a clean slate so callers do not
        // have to "clear" the downloader with a throwaway call first. A cancel
        // that races with this start is re-applied by the owner once the run
        // reports its first state (see download_scheduler::impl::on_state).
        cancelled_.store(false, std::memory_order_relaxed);

        auto result = co_await net::co_spawn(
            executor_,
            [this, url, save_path, headers]() -> net::awaitable<boost::system::error_code>
            { co_return co_await run_download(url, save_path, headers); },
            net::bind_cancellation_slot(signal->slot(), net::use_awaitable));

        cancel_signal_.store(nullptr);
        co_return result;
    }

    net::awaitable<boost::system::error_code>
    downloader::impl::run_download(std::string_view url, fs::path const& save_path, httplib::headers const& headers)
    {
        // Make this run's I/O cancellable with a *total* (graceful) request, and
        // stop the coroutine from throwing on cancellation. In-flight operations
        // then return operation_aborted so the body below still runs its cleanup
        // (disk_.close(), state updates) before returning. The cancellation state
        // is shared by every coroutine in this run's thread of execution, so the
        // helper coroutines (send_request, download_single) inherit it too.
        co_await net::this_coro::reset_cancellation_state(net::enable_total_cancellation());
        co_await net::this_coro::throw_if_cancelled(false);

        downloader::config const cfg = config_.load();
        std::string const auth_scope = cache_manager::auth_scope(headers);

        auto r = url::parse_url(url);
        if (!r)
        {
            auto ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            progress_.set_state(downloader::state::failed, ec);
            co_return ec;
        }
        auto ui = *r;

        std::string const state_url = cache_manager::make_key(ui, auth_scope);

        progress_.set_state(downloader::state::connecting, {});

        // set_state() above runs user/owner callbacks synchronously, which is
        // where a cancellation that raced past the reset is re-applied. Honour
        // it before doing any work (including serving from cache).
        if (cancelled_.load(std::memory_order_relaxed))
        {
            auto aborted = boost::asio::error::operation_aborted;
            progress_.set_state(downloader::state::cancelled, aborted);
            co_return aborted;
        }

        boost::system::error_code ec;
        try
        {
            if (co_await try_serve_from_cache(ui, state_url, save_path, headers))
            {
                co_return boost::system::error_code {};
            }

            // HEAD probe: grab the origin's headers ahead of the GET so its
            // Content-Disposition can name the file and its cache directives can
            // gate whether the fetched body is worth storing.
            httplib::headers probe_headers;
            if (auto head = co_await send_request(ui, method::head, headers);
                head.has_value() && head->response.result() == status::ok)
            {
                // headers() 是 borrow 视图：必须 merge 出副本，否则协程帧销毁后悬垂。
                probe_headers.merge(head->response.headers());
            }

            store_suggested_filename(probe_headers);

            progress_.set_state(downloader::state::downloading, {});

            auto dl = co_await download_single(ui, save_path, cfg, headers);
            if (!dl.has_value())
            {
                ec = dl.error();
            }
            else if (cache_manager_.enabled() && !save_path.empty())
            {
                bool cacheable = http_header_util::response_is_cacheable(probe_headers);
                if (!dl->headers.empty())
                {
                    cacheable = cacheable && http_header_util::response_is_cacheable(dl->headers);
                }
                if (cacheable)
                {
                    auto meta = cache_manager::make_meta(dl->headers, probe_headers, dl->final_ui, true);
                    // Retention is governed by the cache's max_age (so stale
                    // entries stay available for revalidation); HTTP freshness
                    // travels inside the opaque metadata blob.
                    cache_manager_.put(state_url, save_path, meta);
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
                progress_.set_state(downloader::state::cancelled, ec);
            }
            else
            {
                progress_.set_state(downloader::state::failed, ec);
            }
            co_return ec;
        }
        progress_.set_state(downloader::state::completed, {});
        co_return boost::system::error_code {};
    }

    // =========================================================================
    // single-stream download
    // =========================================================================

    net::awaitable<boost::system::result<downloader::impl::download_payload>>
    downloader::impl::download_single(url::url_info const& ui,
                                      fs::path const& save_path,
                                      downloader::config const& cfg,
                                      httplib::headers const& base_headers)
    {
        for (int attempt = 0; attempt <= cfg.max_retries; ++attempt)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }

            auto pause_ec = co_await progress_.wait_if_paused(cancelled_);
            if (pause_ec)
            {
                co_return pause_ec;
            }

            std::uint64_t existing_size = 0;
            httplib::headers req_headers = base_headers;

            if (cfg.resume && attempt == 0)
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
                req_headers.set(field::range, std::format("bytes={}-", existing_size));
            }

            auto result = co_await send_request(ui, method::get, std::move(req_headers));
            if (!result.has_value())
            {
                if (attempt == cfg.max_retries)
                {
                    co_return result.error() ? result.error()
                                             : boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                co_await httplib::util::sleep(cfg.retry_backoff * (attempt + 1));
                continue;
            }

            auto status = result->response.result();
            if (status != status::ok && status != status::partial_content)
            {
                if (attempt == cfg.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                if (!cfg.resume)
                {
                    std::error_code ec;
                    fs::remove(save_path, ec);
                }
                co_await httplib::util::sleep(cfg.retry_backoff * (attempt + 1));
                continue;
            }

            auto content_length = result->response.content_length().value_or(0);
            auto content_range_total = http_header_util::parse_content_range_total(result->response.headers());
            if (status == status::ok && existing_size > 0)
            {
                existing_size = 0;
            }
            else if (status == status::partial_content)
            {
                // Guard against a server that returns 206 with an unexpected
                // starting offset, which would corrupt the appended data.
                if (auto start = http_header_util::parse_content_range_start(result->response.headers());
                    start.has_value() && *start != existing_size)
                {
                    if (attempt == cfg.max_retries)
                    {
                        co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                    }
                    std::error_code ec;
                    fs::remove(save_path, ec);
                    existing_size = 0;
                    co_await httplib::util::sleep(cfg.retry_backoff * (attempt + 1));
                    continue;
                }
            }
            auto file_total = content_range_total > 0 ? content_range_total : (content_length + existing_size);

            progress_.start(file_total);
            progress_.set_downloaded(existing_size);

            if (auto open_ec = co_await disk_.open(save_path, existing_size == 0, existing_size); open_ec)
            {
                co_return open_ec;
            }

            auto& resp = result->response;
            std::vector<char> buf(kReadBufSize);
            std::uint64_t session_bytes = 0;

            while (!resp.is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_await disk_.close();
                    co_return boost::asio::error::operation_aborted;
                }

                auto inner_pause_ec = co_await progress_.wait_if_paused(cancelled_);
                if (inner_pause_ec)
                {
                    co_await disk_.close();
                    co_return inner_pause_ec;
                }
                boost::system::error_code ec;
                auto n = co_await resp.read_some_decompressed(net::buffer(buf), ec);
                if (ec)
                {
                    co_await disk_.close();
                    co_return ec;
                }
                if (n == 0)
                {
                    break;
                }

                if (auto write_ec = co_await disk_.write(std::span<char const>(buf.data(), n)); write_ec)
                {
                    co_await disk_.close();
                    co_return write_ec;
                }

                session_bytes += n;
                progress_.update(n);
            }

            co_await disk_.close();

            // A response that stops short of its declared size must not be
            // reported as a successful download: the file would be silently
            // truncated. Retry (resuming from what we kept), and only fail once
            // the retry budget is exhausted.
            bool const validate_size = !http_header_util::response_is_encoded(result->response.headers());
            if (validate_size && file_total > existing_size && session_bytes != file_total - existing_size)
            {
                if (attempt == cfg.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::message_size);
                }
                co_await httplib::util::sleep(cfg.retry_backoff * (attempt + 1));
                continue;
            }

            store_suggested_filename(result->response.headers());

            progress_.finish();

            // headers() 是 borrow 视图：merge 出副本后再随 payload 离开协程帧。
            download_payload payload;
            payload.headers.merge(result->response.headers());
            payload.final_ui = result->final_ui;
            co_return payload;
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
    }

    // =========================================================================
    // downloader public API
    // =========================================================================

    downloader::downloader(net::io_context& ex, std::shared_ptr<http_client_pool> pool)
        : impl_(std::make_shared<impl>(ex.get_executor(), std::move(pool)))
    {
    }

    downloader::downloader(net::any_io_executor const& ex, std::shared_ptr<http_client_pool> pool)
        : impl_(std::make_shared<impl>(ex, std::move(pool)))
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

    downloader::config
    downloader::get_config() const
    {
        return impl_->get_config();
    }

    net::awaitable<boost::system::error_code>
    downloader::async_download(std::string_view url, fs::path const& save_path, httplib::headers const& headers)
    {
        co_return co_await impl_->async_download(url, save_path, headers);
    }

    std::future<boost::system::error_code>
    downloader::download(std::string_view url, fs::path const& save_path, httplib::headers const& headers)
    {
        return impl_->download(url, save_path, headers);
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
