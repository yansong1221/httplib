#include "downloader_impl.h"
#include "http_header_util.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/url/url.hpp"
#include "progress_tracker.hpp"
#include <algorithm>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

namespace httplib::client
{

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
        cache_manager_ = c ? std::make_unique<cache_manager>(std::move(c)) : nullptr;
    }

    std::shared_ptr<cache>
    downloader::impl::get_cache() const
    {
        return cache_manager_ ? cache_manager_->raw_cache() : nullptr;
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

    void
    downloader::impl::record_final_ui(url::url_info const& ui)
    {
        std::lock_guard lk(resource_mutex_);
        final_ui_ = ui;
        has_final_ui_ = true;
    }

    void
    downloader::impl::record_resource_headers(httplib::headers const& headers)
    {
        std::lock_guard lk(resource_mutex_);
        resource_headers_ = headers;
    }

    // =========================================================================
    // cache helpers
    // =========================================================================

    net::awaitable<bool>
    downloader::impl::check_remote_cache(url::url_info const& ui, cache_manager::http_meta const& meta)
    {
        if (!cache_manager_ || !cache_manager_->enabled())
        {
            co_return false;
        }
        httplib::headers req_headers;
        if (meta.etag.empty() && meta.last_modified.empty())
        {
            // No validator: there is no way to prove the cached body is still
            // current, so never serve it.
            co_return false;
        }
        if (!meta.etag.empty())
        {
            req_headers.set(field::if_none_match, meta.etag);
        }
        if (!meta.last_modified.empty())
        {
            req_headers.set(field::if_modified_since, meta.last_modified);
        }
        auto result = co_await send_request(ui, method::head, req_headers);
        if (result.status != status::not_modified)
        {
            co_return false;
        }
        // Revalidate the final origin too: a URL that now redirects elsewhere
        // must not be served from an entry recorded against the old target.
        if (!meta.final_url.empty() && result.final_ui.to_url() != meta.final_url)
        {
            co_return false;
        }
        co_return true;
    }

    // =========================================================================
    // probe Content-Length
    // =========================================================================

    net::awaitable<downloader::impl::probe_result>
    downloader::impl::probe_content_length(url::url_info const& ui)
    {
        probe_result res;
        auto result = co_await send_request(ui, method::head);
        if (result.status == status::ok)
        {
            // 必须深拷贝：result.headers 只是 result.response 的借用视图，
            // 而 result 会在 co_return 后随协程帧一起销毁。
            res.headers.merge(result.headers);
            res.content_length = result.response.content_length().value_or(0);
        }
        co_return res;
    }

    // =========================================================================
    // send_request (with redirect)
    // =========================================================================

    net::awaitable<request_sender::result>
    downloader::impl::send_request(url::url_info const& ui, httplib::method m, httplib::headers const& req_headers)
    {
        if (!sender_)
        {
            request_sender::result rr;
            rr.error = boost::asio::error::operation_aborted;
            co_return rr;
        }
        auto rr = co_await sender_->send(ui, m, req_headers, cancelled_, per_connection_rate_);
        if (rr.handle)
        {
            record_final_ui(rr.final_ui);
        }
        co_return rr;
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
        // helper coroutines (send_request, co_download_*) inherit it too.
        co_await net::this_coro::reset_cancellation_state(net::enable_total_cancellation());
        co_await net::this_coro::throw_if_cancelled(false);

        downloader::config const cfg = config_.load();
        per_connection_rate_ = cfg.max_speed_bytes_per_sec;
        httplib::headers const custom_headers = headers;
        std::string const auth_scope = cache_manager::auth_scope(custom_headers);

        request_sender::config sender_cfg;
        sender_cfg.acquire_timeout = cfg.acquire_timeout;
        sender_cfg.timeout = cfg.timeout;
        sender_cfg.verify_ssl = cfg.verify_ssl;
        sender_cfg.max_redirects = cfg.max_redirects;
        sender_ = std::make_unique<request_sender>(pool_, custom_headers, sender_cfg);
        {
            std::lock_guard lk(resource_mutex_);
            resource_headers_.clear();
            final_ui_ = {};
            has_final_ui_ = false;
        }

        auto r = url::parse_url(url);
        if (!r)
        {
            auto ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            progress_.set_state(downloader::state::failed, ec);
            co_return ec;
        }
        auto ui = *r;

        std::string const state_url = cache_manager::make_key(ui, auth_scope);

        strategy_ = std::make_unique<download_strategy>(
            disk_,
            progress_,
            cancelled_,
            cfg,
            [this](url::url_info const& u, httplib::method m, httplib::headers const& h)
                -> net::awaitable<request_sender::result> { co_return co_await send_request(u, m, h); },
            [this](httplib::headers const& h) { store_suggested_filename(h); },
            [this](httplib::headers const& h) { record_resource_headers(h); },
            state_url);

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
            if (cache_manager_ && cache_manager_->enabled())
            {
                auto entry = cache_manager_->get(state_url);
                if (entry.has_value())
                {
                    auto meta = cache_manager::parse_meta(entry->metadata);
                    if (meta.has_value())
                    {

                        progress_.set_state(downloader::state::downloading, {});

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
                            auto fresh = cache_manager_->get(state_url);
                            if (fresh.has_value())
                            {
                                std::error_code copy_ec;
                                fs::copy_file(fresh->body_path,
                                              save_path,
                                              fs::copy_options::overwrite_existing,
                                              copy_ec);
                                if (!copy_ec)
                                {
                                    progress_.finish_with_bytes(fresh->body_size);
                                    progress_.set_state(downloader::state::completed, {});
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

            progress_.set_state(downloader::state::downloading, {});

            if (content_length > 0 && cfg.segments > 1)
            {
                // Spread the configured cap over the concurrent segment
                // connections so the aggregate stays near the requested rate.
                if (per_connection_rate_ > 0)
                {
                    auto segs = static_cast<std::uint64_t>(std::clamp(cfg.segments, 2, 32));
                    per_connection_rate_ = std::max<std::uint64_t>(1, per_connection_rate_ / segs);
                }
                ec = co_await strategy_->download_multi(ui, save_path, content_length, probe.headers);
                if (ec == boost::system::errc::make_error_code(boost::system::errc::operation_not_supported))
                {
                    // Server does not honor Range requests; fall back to a plain
                    // single-stream download.
                    per_connection_rate_ = cfg.max_speed_bytes_per_sec;
                    ec = co_await strategy_->download_single(ui, save_path);
                }
            }
            else
            {
                ec = co_await strategy_->download_single(ui, save_path);
            }

            if (!ec && cache_manager_ && cache_manager_->enabled() && !save_path.empty())
            {
                httplib::headers response_headers;
                url::url_info final_ui;
                bool has_final = false;
                {
                    std::lock_guard lk(resource_mutex_);
                    response_headers = resource_headers_;
                    final_ui = final_ui_;
                    has_final = has_final_ui_;
                }
                bool cacheable = http_header_util::response_is_cacheable(probe.headers);
                if (!response_headers.empty())
                {
                    cacheable = cacheable && http_header_util::response_is_cacheable(response_headers);
                }
                if (cacheable)
                {
                    auto meta = cache_manager::make_meta(response_headers, probe.headers, final_ui, has_final);
                    // Retention is governed by the cache's max_age (so stale
                    // entries stay available for revalidation); HTTP freshness
                    // travels inside the opaque metadata blob.
                    cache_manager_->put(state_url, save_path, meta);
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
