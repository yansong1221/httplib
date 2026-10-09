#include "download_strategy.hpp"
#include "http_header_util.hpp"
#include "httplib/util/sleep.hpp"
#include "httplib/util/when_all.hpp"
#include <algorithm>
#include <boost/asio/error.hpp>
#include <format>
#include <span>

namespace httplib::client
{
    namespace
    {
        constexpr std::size_t kReadBufSize = 64 * 1024;
    } // namespace

    download_strategy::download_strategy(disk_writer& disk,
                                         progress_tracker& progress,
                                         std::atomic<bool>& cancelled,
                                         downloader::config cfg,
                                         send_fn send,
                                         headers_sink on_suggested_filename,
                                         headers_sink on_resource_headers,
                                         std::string state_url)
        : disk_(disk)
        , progress_(progress)
        , cancelled_(cancelled)
        , cfg_(cfg)
        , send_(std::move(send))
        , on_suggested_filename_(std::move(on_suggested_filename))
        , on_resource_headers_(std::move(on_resource_headers))
        , state_url_(std::move(state_url))
    {
    }

    net::awaitable<request_sender::result>
    download_strategy::send(url::url_info const& ui, httplib::method m, httplib::headers const& req_headers)
    {
        co_return co_await send_(ui, m, req_headers);
    }

    // =========================================================================
    // single-segment download
    // =========================================================================

    net::awaitable<boost::system::error_code>
    download_strategy::download_single(url::url_info const& ui, fs::path const& save_path)
    {
        for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt)
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
            httplib::headers req_headers;

            if (cfg_.resume && attempt == 0)
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

            auto result = co_await send(ui, method::get, req_headers);
            if (!result.handle)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return result.error ? result.error
                                           : boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            auto status = result.status;
            if (status != status::ok && status != status::partial_content)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                if (!cfg_.resume)
                {
                    std::error_code ec;
                    fs::remove(save_path, ec);
                }
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            auto content_length = result.response.content_length().value_or(0);
            auto content_range_total = http_header_util::parse_content_range_total(result.headers);
            if (status == status::ok && existing_size > 0)
            {
                existing_size = 0;
            }
            else if (status == status::partial_content)
            {
                // Guard against a server that returns 206 with an unexpected
                // starting offset, which would corrupt the appended data.
                if (auto start = http_header_util::parse_content_range_start(result.headers);
                    start.has_value() && *start != existing_size)
                {
                    if (attempt == cfg_.max_retries)
                    {
                        co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                    }
                    std::error_code ec;
                    fs::remove(save_path, ec);
                    existing_size = 0;
                    co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                    continue;
                }
            }
            auto file_total = content_range_total > 0 ? content_range_total : (content_length + existing_size);

            progress_.start(file_total, 1);
            progress_.set_downloaded(existing_size);

            if (auto open_ec = co_await disk_.open(save_path, existing_size == 0, 0, existing_size); open_ec)
            {
                co_return open_ec;
            }

            auto& resp = result.response;
            std::vector<char> buf(kReadBufSize);
            std::uint64_t session_bytes = 0;

            while (!resp.is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_await disk_.close();
                    co_return boost::asio::error::operation_aborted;
                }

                auto pause_ec = co_await progress_.wait_if_paused(cancelled_);
                if (pause_ec)
                {
                    co_await disk_.close();
                    co_return pause_ec;
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
            bool const validate_size = !http_header_util::response_is_encoded(result.headers);
            if (validate_size && file_total > existing_size && session_bytes != file_total - existing_size)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::message_size);
                }
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            on_suggested_filename_(result.headers);
            on_resource_headers_(result.headers);

            progress_.finish();

            co_return boost::system::error_code {};
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
    }

    // =========================================================================
    // segment download
    // =========================================================================

    net::awaitable<boost::system::error_code>
    download_strategy::download_segment(url::url_info const& ui, std::uint64_t start, std::uint64_t end, int index)
    {
        auto const seg_len = end - start + 1;

        for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt)
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

            std::uint64_t have = progress_.segment_bytes(index);
            if (have >= seg_len)
            {
                // Already fully persisted (previous attempt or run).
                co_return boost::system::error_code {};
            }

            std::uint64_t const resume_at = start + have;
            httplib::headers req_headers;
            req_headers.set(field::range, std::format("bytes={}-{}", resume_at, end));

            auto result = co_await send(ui, method::get, req_headers);
            if (!result.handle)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return result.error ? result.error
                                           : boost::system::errc::make_error_code(boost::system::errc::timed_out);
                }
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            if (result.status == status::ok)
            {
                // The server ignored the Range request: byte ranges are not
                // supported, so segmented downloading cannot proceed. Signal the
                // caller to fall back to a single-segment download.
                co_return boost::system::errc::make_error_code(boost::system::errc::operation_not_supported);
            }
            if (result.status != status::partial_content)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            // Guard against a server returning a range that does not start where
            // we asked; writing it at our own offset would corrupt the file.
            if (auto range_start = http_header_util::parse_content_range_start(result.headers);
                range_start.has_value() && *range_start != resume_at)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::protocol_error);
                }
                // The remote layout disagrees with ours: restart this segment
                // from its beginning, overwriting whatever it had.
                progress_.reset_segment(index);
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            on_suggested_filename_(result.headers);
            on_resource_headers_(result.headers);

            auto& resp = result.response;
            std::vector<char> buf(kReadBufSize);
            std::uint64_t session_bytes = 0;

            while (!resp.is_body_done())
            {
                if (cancelled_.load(std::memory_order_relaxed))
                {
                    co_return boost::asio::error::operation_aborted;
                }

                auto inner_pause_ec = co_await progress_.wait_if_paused(cancelled_);
                if (inner_pause_ec)
                {
                    co_return inner_pause_ec;
                }
                boost::system::error_code ec;
                auto n = co_await resp.read_some_decompressed(net::buffer(buf), ec);
                if (ec)
                {
                    co_return ec;
                }
                if (n == 0)
                {
                    break;
                }

                if (auto write_ec
                    = co_await disk_.write_at(resume_at + session_bytes, std::span<char const>(buf.data(), n));
                    write_ec)
                {
                    co_return write_ec;
                }

                session_bytes += n;
                progress_.record_segment_bytes(index, n);
                progress_.update(n);
            }

            // The segment must have received exactly the bytes it asked for.
            // A short read (server closed early or lied about the range) would
            // otherwise be counted as complete.
            auto expected = end - resume_at + 1;
            if (!http_header_util::response_is_encoded(result.headers) && session_bytes != expected)
            {
                if (attempt == cfg_.max_retries)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::message_size);
                }
                co_await httplib::util::sleep(cfg_.retry_backoff * (attempt + 1));
                continue;
            }

            co_return boost::system::error_code {};
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::timed_out);
    }

    // =========================================================================
    // multi-segment orchestration
    // =========================================================================

    net::awaitable<boost::system::error_code>
    download_strategy::download_multi(url::url_info const& ui,
                                      fs::path const& save_path,
                                      std::uint64_t content_length,
                                      httplib::headers const& probe_headers)
    {
        int seg_count = cfg_.segments;
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
        segments_.reserve(static_cast<std::size_t>(seg_count));

        std::uint64_t offset = 0;
        for (int i = 0; i < seg_count; ++i)
        {
            auto sz = seg_size + (static_cast<std::uint64_t>(i) < remainder ? 1 : 0);
            std::uint64_t start_byte = offset;
            std::uint64_t end_byte = offset + sz - 1;
            offset += sz;

            segments_.push_back({ start_byte, end_byte, i });
        }

        // A sidecar state file pins down the layout (url + content length +
        // segment count) and the per-segment progress, so stale or inconsistent
        // state from another download is never resumed.
        bool can_resume = false;
        download_state prev;
        if (cfg_.resume && cfg_.save_state)
        {
            prev = state_store(save_path).load();
            if (prev.content_length == content_length && prev.segments == seg_count && prev.url == state_url_)
            {
                can_resume = true;
                for (int i = 0; i < seg_count; ++i)
                {
                    auto uidx = static_cast<std::size_t>(i);
                    auto seg_len = segments_[uidx].end_byte - segments_[uidx].start_byte + 1;
                    auto have = uidx < prev.seg_downloaded.size() ? prev.seg_downloaded[uidx] : 0;
                    // A counter past its segment means the state is corrupt;
                    // abandon the resume rather than skip bytes we may not have.
                    if (have > seg_len)
                    {
                        can_resume = false;
                        break;
                    }
                }
            }
        }

        // Open the single output file all segments write into. A resumed run
        // keeps existing bytes; a fresh run truncates and preallocates so that
        // out-of-order segment writes never zero-fill a gap.
        if (auto open_ec = co_await disk_.open(save_path, !can_resume, content_length, 0); open_ec)
        {
            co_return open_ec;
        }

        // start() resets per-segment counters, so seed the resumed progress
        // *after* it or the restored bytes are wiped.
        progress_.start(content_length, seg_count);
        if (can_resume)
        {
            for (int i = 0; i < seg_count; ++i)
            {
                auto uidx = static_cast<std::size_t>(i);
                auto v = uidx < prev.seg_downloaded.size() ? prev.seg_downloaded[uidx] : 0;
                progress_.set_segment_bytes(i, v);
            }
        }

        std::uint64_t already_downloaded = 0;
        for (int i = 0; i < seg_count; ++i)
        {
            already_downloaded += progress_.segment_bytes(i);
        }
        progress_.set_downloaded(already_downloaded);

        on_suggested_filename_(probe_headers);

        // Persist the layout up-front so an interrupted (e.g. crashed) run can be
        // resumed on the next attempt.
        save_state(save_path);

        progress_.notify_initial(content_length, already_downloaded, seg_count);

        std::vector<net::awaitable<boost::system::error_code>> ops;
        ops.reserve(static_cast<std::size_t>(seg_count));

        for (auto& seg : segments_)
        {
            ops.emplace_back(download_segment(ui, seg.start_byte, seg.end_byte, seg.index));
        }

        boost::system::error_code first_error;
        boost::system::error_code aborted_error;
        bool ranges_unsupported = false;

        try
        {
            auto errors = co_await util::when_all(std::move(ops));
            progress_.set_active_segments(0);
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

        co_await disk_.close();

        if (first_error)
        {
            // The single output file is unusable; drop it and the state so the
            // next run starts over instead of failing forever.
            std::error_code rm_ec;
            fs::remove(save_path, rm_ec);
            state_store(save_path).clear();
            co_return first_error;
        }

        if (ranges_unsupported)
        {
            // Byte ranges are unavailable; leave nothing behind and let the
            // caller retry as a single stream.
            std::error_code rm_ec;
            fs::remove(save_path, rm_ec);
            state_store(save_path).clear();
            co_return boost::system::errc::make_error_code(boost::system::errc::operation_not_supported);
        }

        if (aborted_error)
        {
            // Keep the output + per-segment progress so a later run can resume.
            save_state(save_path);
            co_return aborted_error;
        }

        state_store(save_path).clear();

        co_return boost::system::error_code {};
    }

    void
    download_strategy::save_state(fs::path const& save_path)
    {
        if (!cfg_.save_state)
        {
            return;
        }
        std::vector<std::uint64_t> counts;
        counts.reserve(static_cast<std::size_t>(segments_.size()));
        for (int i = 0; i < static_cast<int>(segments_.size()); ++i)
        {
            counts.push_back(progress_.segment_bytes(i));
        }
        state_store(save_path).save(state_url_, progress_.total_bytes(), counts);
    }

} // namespace httplib::client
